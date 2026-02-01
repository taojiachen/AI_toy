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
/*--------------------------------------opus_decoder_start-----------------------------------------------*/

// 1. 提前声明解码器结构体（解决循环依赖）为 audio_decoder 的结构体类型起一个简写别名 audio_decoder_t
typedef struct audio_decoder audio_decoder_t;

enum {
    DECODER_OK = 0,
    DECODER_HEADER_ONLY,
    DECODER_EOF,
    DECODER_ERROR,
    DECODER_NEED_MORE_DATA
} typedef decoder_result_t;

// 音频信息结构体
typedef struct {
    uint32_t sample_rate; // 采样率
    uint8_t channels;     // 声道数
} audio_decoder_info_t;

typedef enum {
    AUDIO_DATA_TYPE_FILE = 0,
    AUDIO_DATA_TYPE_BUFFER = 1,
} audio_data_type_t;

// 2. 内存缓冲区子结构体（单独命名，提升可读性）
typedef struct {
    uint8_t *buffer;    // 音频数据缓冲区首地址
    size_t data_size;   // 缓冲区中有效音频数据的字节数
} audio_buffer_t;

// 3. 音频数据源主结构体（优化联合体命名，增加注释）
typedef struct {
    audio_data_type_t type;  // 数据源类型（决定联合体使用哪个成员）
    union {
        FILE *file;          // 当type=AUDIO_DATA_TYPE_FILE时使用
        audio_buffer_t buf;  // 当type=AUDIO_DATA_TYPE_BUFFER时使用
    } data;                  // 联合体命名为data，明确是数据源内容
} data_source_t;

// // 音频设备结构体（公共）
// typedef struct {
//     bool is_file_end;                 // 文件播放结束标志
//     bool is_transimitting;            // 传输中标志
//     bool is_playing;                  // 播放中标志
//     const audio_decoder_t *decoder;   // 解码器实例（关键：绑定具体解码器）
// } device_audio_t;

// 解码器抽象接口（核心）
struct audio_decoder{
    void * context;
    audio_decoder_info_t info;
    decoder_result_t (*init)(struct audio_decoder *decoder);
    decoder_result_t (*decode_frame)(struct audio_decoder *decoder, data_source_t *source, int16_t *output, uint32_t *samples_decoded);
    void (*deinit)(struct audio_decoder *decoder);
};

/*--------------------------------------opus_decoder_end-----------------------------------------------*/

/*--------------------------------------opus_encoder_start-----------------------------------------------*/

typedef struct audio_encoder audio_encoder_t;

typedef enum {
    ENCODER_OK = 0,        // 编码成功
    ENCODER_ERROR = -1,    // 编码错误
    ENCODER_EMPTY = -2,    // 输入数据为空
    ENCODER_TOO_BIG = -3   // 输出缓冲区不足
} encoder_result_t;

// 音频信息结构体
typedef struct {
    uint32_t sample_rate;     // 采样率
    uint8_t channels;         // 声道数
    uint32_t bitrate;          // 比特率（bps）
    uint32_t frame_samples;    // 每帧采样数（根据时长计算）
} audio_encoder_info_t;

// // 音频设备结构体（公共）
// typedef struct {
//     bool is_encoder_end;              // 编码器结束标志
//     bool is_transimitting;            // 传输中标志
//     bool is_playing;                  // 播放中标志
//     const audio_decoder_t *decoder;   // 解码器实例（关键：绑定具体解码器）
// } device_audio_t;

// 解码器抽象接口（核心）
struct audio_encoder{
    void * context;
    audio_encoder_info_t info;
    encoder_result_t (*init)(struct audio_encoder *encoder);
    encoder_result_t (*encode_frame)(struct audio_encoder *encoder, const int16_t *input, uint32_t input_samples, uint8_t *output, uint32_t output_samples, uint16_t *bytes_encoded);
    void (*deinit)(struct audio_encoder *encoder);
};

/*--------------------------------------opus_encoder_end-----------------------------------------------*/

#endif /* __AUDIO_PRIVATE_H__ */