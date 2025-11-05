#ifndef __AUDIO_H__
#define __AUDIO_H__

#include "device.h"
#include "i2s.h"
#include "fs.h"
#include "osal.h"

// 音频源类型
typedef enum {
    AUDIO_SRC_UNKNOWN,
    AUDIO_SRC_FILE,      // 文件路径
    AUDIO_SRC_VARIABLE   // 内存数据
} audio_src_type_t;

// 音频信息结构体
typedef struct {
    const char *format;   // 格式（mp3/opus）
    uint32_t sample_rate; // 采样率
    uint8_t channels;     // 声道数
    uint32_t bitrate;     // 比特率
    uint32_t duration;    // 时长(ms)
} audio_info_t;

// 音频数据描述符（内存播放用）
typedef struct {
    uint8_t *data;        // 音频数据
    uint32_t data_size;   // 数据大小
} audio_dsc_t;

// 解码器抽象接口（核心）
typedef struct {
    // 初始化解码器
    int (*init)(void **priv_data);
    // 播放文件
    void (*play_file)(void *priv_data, device_audio_t *dev, const char *path);
    // 播放内存数据
    void (*play_variable)(void *priv_data, device_audio_t *dev, audio_dsc_t *dsc);
    // 获取音频信息
    audio_info_t* (*get_info)(void *priv_data);
    // 关闭解码器
    void (*close)(void *priv_data);
} audio_decoder_t;

// 音频设备结构体（公共）
typedef struct {
    device_t parent;                  // 继承设备基类
    struct {
        device_t *i2s_bus;            // I2S总线设备
        const char *i2s_bus_name;     // I2S总线名称
        struct i2c_device_config i2c_cfg; // I2C配置
        device_t *i2c_bus;            // I2C总线设备
        struct device_pin_info spk_pin_get; // 扬声器引脚信息
        device_t *spk_en_pin;         // 扬声器使能引脚设备
        struct device_pin_info rst_pin_get; // 复位引脚信息
        device_t *rst_pin;            // 复位引脚设备
    } info;
    osal_sema_t *sem_stop;            // 停止信号量
    bool is_file_end;                 // 文件播放结束标志
    bool is_transimitting;            // 传输中标志
    bool is_playing;                  // 播放中标志
    audio_eof_cb_func eof_cb;         // 播放结束回调
    const audio_decoder_t *decoder;   // 解码器实例（关键：绑定具体解码器）
    void *decoder_priv;               // 解码器私有数据
} device_audio_t;

// 公共函数声明
audio_src_type_t audio_src_get_type(const void *src);
int audio_speaker_enable_pin_init(device_audio_t *device);
int audio_reset_pin_init(device_audio_t *device);
int audio_i2c_init(device_audio_t *device);
int audio_i2s_init(device_audio_t *device);
void audio_set_volume(device_t *dev, uint8_t volume);
void audio_play(device_t *dev, const void *src, uint8_t volume);
os_err_t device_audio_register(device_audio_t *device, const char *name, const audio_decoder_t *decoder);

#endif /* __AUDIO_H__ */