#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "websocket.h"

// 确保WS_SERVER宏定义正确，例如：
// #define WS_SERVER "ws://your_server_ip:port/path"

static struct mg_mgr mgr;
static struct mg_connection *ws_conn = NULL; // 保存WebSocket连接句柄
static TaskHandle_t mg_task_handle;

// WebSocket事件回调处理
static void ws_event_handler(struct mg_connection *c, int ev, void *ev_data)
{
  switch (ev)
  {
  // TCP连接成功事件（此时还未进行WebSocket握手）
  case MG_EV_CONNECT:
  {
    int status = *(int *)ev_data;
    if (status == 0)
    {
      printf("TCP connected, starting WebSocket handshake...\n");
      // 无需手动发起握手，mg_ws_connect会自动处理
    }
    else
    {
      printf("TCP connect failed: %d. Reconnecting...\n", status);
    }
    break;
  }

  // WebSocket握手成功事件（连接就绪）
  case MG_EV_WS_OPEN:
    ws_conn = c; // 保存连接句柄
    printf("WebSocket handshake done! Full-duplex ready.\n");
    // 发送初始化消息
    mg_ws_send(c, "ESP32-S3 WS Ready", strlen("ESP32-S3 WS Ready"), WEBSOCKET_OP_TEXT);
    break;

  // 收到WebSocket消息事件
  case MG_EV_WS_MSG:
  {
    struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
    // 处理文本帧
    if ((wm->flags & 0x0F) == WEBSOCKET_OP_TEXT)
    {
      char recv_buf[1024] = {0};
      size_t copy_len = wm->data.len < sizeof(recv_buf) - 1 ? wm->data.len : sizeof(recv_buf) - 1;
      strncpy(recv_buf, (char *)wm->data.buf, copy_len);
      printf("Received from Server: %s\n", recv_buf);

      // 回复收到的消息
      char reply_buf[128];
      snprintf(reply_buf, sizeof(reply_buf), "ESP32 Got: %s", recv_buf);
      mg_ws_send(c, reply_buf, strlen(reply_buf), WEBSOCKET_OP_TEXT);
    }
    break;
  }

  // 连接关闭事件
  case MG_EV_CLOSE:
    if (ws_conn == c)
    { // 确认是当前WebSocket连接
      ws_conn = NULL;
      printf("WebSocket closed. Reconnect in 3s...\n");
      // 延迟后重新连接
      vTaskDelay(pdMS_TO_TICKS(3000));
      mg_ws_connect(&mgr, WS_SERVER, ws_event_handler, NULL, NULL);
    }
    break;

  // 错误事件
  case MG_EV_ERROR:
    printf("Error: %s\n", (char *)ev_data);
    break;
  }
}

// Mongoose事件循环任务
static void mg_task(void *arg)
{
  // 初始化事件管理器
  mg_mgr_init(&mgr);
  printf("Mongoose init done. Connecting to WebSocket server...\n");

  // 发起WebSocket连接（自动处理握手）
  mg_ws_connect(&mgr, WS_SERVER, ws_event_handler, NULL, NULL);

  // 事件循环
  for (;;)
  {
    mg_mgr_poll(&mgr, 1000); // 每1秒轮询一次

    // 周期性发送心跳（仅在连接有效时）
    static uint32_t last_heartbeat = 0;
    if (ws_conn != NULL)
    {
      uint32_t now = xTaskGetTickCount();
      if (now - last_heartbeat > pdMS_TO_TICKS(5000))
      {
        last_heartbeat = now;
        mg_ws_send(ws_conn, "ESP32 Heartbeat", strlen("ESP32 Heartbeat"), WEBSOCKET_OP_TEXT);
      }
    }
  }

  // 理论上不会执行到这里，仅作为清理示例
  mg_mgr_free(&mgr);
  vTaskDelete(NULL);
}

// 启动WebSocket任务
void ws_start(void)
{
// 确保任务栈大小和优先级适合你的应用
#define TASK_STACK_SIZE 4096
#define TASK_PRIORITY 5
  xTaskCreate(mg_task, "mongoose_ws_task", TASK_STACK_SIZE, NULL, TASK_PRIORITY, &mg_task_handle);
}