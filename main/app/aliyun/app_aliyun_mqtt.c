#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/adc.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "mqtt_client.h"
#include "aliyun_mqtt.h"
#include "cJSON.h"

#include "esp_system.h"
#include "esp_sntp.h"
#include "esp_pm.h"

#include <event.h>
#include <app_task_list.h>
#include "websocket.h"

static const char *TAG = "APP_ALIYUN_MQTT";

esp_mqtt_client_handle_t client;

// 调用此函数主动请求设备影子
void request_device_shadow(void)
{
    if (client == NULL)
    {
        ESP_LOGE(TAG, "MQTT client not initialized yet");
        return;
    }
    // 发布 get 请求到影子 update topic
    const char *get_payload = "{\"method\":\"get\"}";
    int msg_id = esp_mqtt_client_publish(client, CONFIG_AliYun_SHADOW_UPDATE_TOPIC, get_payload, strlen(get_payload), 1, 0);
    if (msg_id >= 0)
    {
        ESP_LOGI(TAG, "Shadow get request sent, msg_id=%d, topic=%s", msg_id, CONFIG_AliYun_SHADOW_UPDATE_TOPIC);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to send shadow get request");
    }
}

static esp_err_t app_mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    client = event->client;

    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

        // 订阅设备影子响应 Topic
        int msg_id = esp_mqtt_client_subscribe(client, CONFIG_AliYun_SHADOW_GET_TOPIC, 1);
        ESP_LOGI(TAG, "Subscribed to shadow get topic: %s, msg_id=%d", CONFIG_AliYun_SHADOW_GET_TOPIC, msg_id);

        // 订阅自定义用户 get Topic（手机可向该 Topic 下发指令）
        msg_id = esp_mqtt_client_subscribe(client, CONFIG_AliYun_SUBSCRIBE_TOPIC_USER_GET, 1);
        ESP_LOGI(TAG, "Subscribed to user get topic: %s, msg_id=%d", CONFIG_AliYun_SUBSCRIBE_TOPIC_USER_GET, msg_id);

        // 订阅属性上报回复 Topic（用于接收平台对属性上报的应答）
        msg_id = esp_mqtt_client_subscribe(client, CONFIG_AliYun_PUBLISH_TOPIC_USER_POST_REPLY, 1);
        ESP_LOGI(TAG, "Subscribed to post reply topic: %s, msg_id=%d", CONFIG_AliYun_PUBLISH_TOPIC_USER_POST_REPLY, msg_id);

        // 请求一次设备影子（获取最新状态）
        request_device_shadow();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        // 注意：这里使用了同样的发布主题，根据实际逻辑可能需要调整
        // msg_id = esp_mqtt_client_publish(client, CONFIG_AliYun_PUBLISH_TOPIC_USER_UPDATE, mqtt_publish_data2, strlen(mqtt_publish_data2), 0, 0);
        // ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
    {
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");

        // 1. 先判断是否是设备影子 Topic（影子数据）
        if (event->topic_len == strlen(CONFIG_AliYun_SHADOW_GET_TOPIC) &&
            memcmp(event->topic, CONFIG_AliYun_SHADOW_GET_TOPIC, event->topic_len) == 0)
        {
            // 影子数据处理：解析 IP 并启动 WebSocket（仅一次）
            ESP_LOGI(TAG, "*************** Received Device Shadow Data ***************");
            ESP_LOGI(TAG, "Shadow Topic: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "Shadow Payload: %.*s", event->data_len, event->data);

            cJSON *root = cJSON_Parse(event->data);
            if (root != NULL)
            {
                // 尝试提取 payload.state.reported.ip
                cJSON *payload = cJSON_GetObjectItem(root, "payload");
                if (payload != NULL)
                {
                    cJSON *state = cJSON_GetObjectItem(payload, "state");
                    if (state != NULL)
                    {
                        cJSON *reported = cJSON_GetObjectItem(state, "reported");
                        if (reported != NULL)
                        {
                            cJSON *ip_json = cJSON_GetObjectItem(reported, "ip");
                            if (cJSON_IsString(ip_json) && ip_json->valuestring != NULL)
                            {
                                const char *new_ip = ip_json->valuestring;
                                ESP_LOGI(TAG, "Extracted IP address: %s", new_ip);

                                // 只调用一次 ws_start
                                static bool ws_started = false;
                                if (!ws_started)
                                {
                                    static char ws_url[64];
                                    snprintf(ws_url, sizeof(ws_url), "wss://%s:8765", new_ip);
                                    ESP_LOGI(TAG, "Connecting to WebSocket server: %s", ws_url);
                                    ws_start(ws_url); // 启动 WebSocket 连接
                                    ws_started = true;
                                }
                                else
                                {
                                    ESP_LOGD(TAG, "WebSocket already started, ignore");
                                }
                            }
                            else
                            {
                                ESP_LOGW(TAG, "IP field not found or not a string");
                            }
                        }
                    }
                }
                cJSON_Delete(root);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to parse shadow JSON");
            }
            // 影子消息处理完毕，不再执行下方的“原来逻辑”
            break;
        }

        // 2. 非影子 Topic 或未命中上述条件，执行原来的业务逻辑
        ESP_LOGE(TAG, "MQTT_EVENT_DATA (original path)");
        const char *jsonStr = event->data;
        ESP_LOGE(TAG, "DATA=%.*s\r\n", event->data_len, jsonStr);
        update_task(jsonStr); // 原来的业务处理函数
        printf("触发app_aliyun_mqtt.c的消息云流转");

        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    case MQTT_EVENT_ANY:
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
    return ESP_OK;
}

static void app_mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%ld", base, event_id);
    app_mqtt_event_handler_cb(event_data);
}

void app_aliyun_mqtt_init(void)
{
    aliyun_mqtt_init(app_mqtt_event_handler);
    // vTaskDelete(NULL);
}