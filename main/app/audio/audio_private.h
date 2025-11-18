#ifndef __AUDIO_PRIVATE_H__
#define __AUDIO_PRIVATE_H__

#include "stdint.h"
#include <stdbool.h>
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "esp_spiffs.h"
/*

#if CONFIG_EASYLOGGER_SUPPORT
#include "elog.h"
#define LOG_ERR log_e
#define LOG_INF log_i
#define LOG_WRN log_w
#define LOG_DBG log_d
#else
#define AUDIO_NORMAL_LOG_OUTPUT do {\
    printf("%s\r\n", __func__);\
    } while (0)
#define LOG_ERR(...) AUDIO_NORMAL_LOG_OUTPUT
#define LOG_INF(...) AUDIO_NORMAL_LOG_OUTPUT
#define LOG_WRN(...) AUDIO_NORMAL_LOG_OUTPUT
#define LOG_DBG(...) AUDIO_NORMAL_LOG_OUTPUT
#endif

*/

// 1. 提前声明解码器结构体（解决循环依赖）
typedef struct audio_decoder audio_decoder_t;

enum {
    DECODER_OK = 0,
    DECODER_HEADER_ONLY,
    DECODER_EOF,
    DECODER_ERROR
} typedef decoder_result_t;

// 音频信息结构体
typedef struct {
    uint32_t sample_rate; // 采样率
    uint8_t channels;     // 声道数
} audio_info_t;

// 音频设备结构体（公共）
typedef struct {
    bool is_file_end;                 // 文件播放结束标志
    bool is_transimitting;            // 传输中标志
    bool is_playing;                  // 播放中标志
    const audio_decoder_t *decoder;   // 解码器实例（关键：绑定具体解码器）
} device_audio_t;

// 解码器抽象接口（核心）
struct audio_decoder{
    // 额外上下文数据
    void * context;
    // 相同上下文数据：音频信息
    audio_info_t info;
    // 初始化解码器
    decoder_result_t (*init)(struct audio_decoder *decoder);
    // 帧解码
    decoder_result_t (*decode_frame)(struct audio_decoder *decoder, FILE *file, int16_t *output, uint32_t *samples_decoded);
    // 关闭解码器
    void (*deinit)(struct audio_decoder *decoder);
};

#endif /* __AUDIO_PRIVATE_H__ */