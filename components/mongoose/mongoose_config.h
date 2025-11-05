#ifndef MONGOOSE_CONFIG_H
#define MONGOOSE_CONFIG_H

// 1. 必须添加：指定 ESP32 平台架构（关键）
#define MG_ARCH MG_ARCH_ESP32  // 3 对应 ESP32/ARM 架构（Mongoose 官方定义）

// 2. 基础功能配置（启用需要的模块，关闭不需要的）
#define MG_ENABLE_FILE 0  // 关闭文件系统（嵌入式无需）
#define MG_TLS 1
#define MG_IO_SIZE 4096  // I/O 缓冲区大小

#endif // MONGOOSE_CONFIG_H