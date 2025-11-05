#ifndef MONGOOSE_CONFIG_H
#define MONGOOSE_CONFIG_H

// 1. 必须添加：指定 ESP32 平台架构（关键）
#define MG_ARCH MG_ARCH_ESP32  // 32 对应 ESP32/ARM 架构（Mongoose 官方定义）
#define MG_PLATFORM "ESP32"  // 可选，标识平台名称

// 2. 基础功能配置（启用需要的模块，关闭不需要的）
#define MG_ENABLE_TCP 1
#define MG_ENABLE_IPV4 1
#define MG_ENABLE_HTTP 1
#define MG_ENABLE_WEBSOCKET 1
#define MG_ENABLE_FILE 0  // 关闭文件系统（嵌入式无需）
#define MG_ENABLE_MBEDTLS 0  // 不启用 SSL（需要时再开）
#define MG_IO_SIZE 4096  // I/O 缓冲区大小

// 3. 适配 ESP32 底层函数（避免函数未定义）
#define MG_MILLIS() (esp_timer_get_time() / 1000)  // 时间函数：毫秒级
#define MG_SOCKET_ERROR (-1)  // 适配 ESP32 socket 错误码

#endif // MONGOOSE_CONFIG_H