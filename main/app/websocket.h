#ifndef __WEBSOCKET_H__
#define __WEBSOCKET_H__

#include <time.h>
#include "mongoose.h"

// WebSocket 服务器地址（根据实际情况修改）
#define WS_SERVER "wss://openspeech.bytedance.com/api/v3/realtime/dialogue"

// 任务配置（ESP32 任务栈大小和优先级）
#define TASK_STACK_SIZE 4096  // 栈大小（字节）
#define TASK_PRIORITY 5       // 优先级（0-24，越高越优先）

void ws_start(void);

#endif /* __WEBSOCKET_H__ */