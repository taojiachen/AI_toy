#ifndef __AUDIO_H__
#define __AUDIO_H__

#include "stdint.h"
#include <stdbool.h>
#include "audio_private.h"

// 公共函数声明
void audio_set_volume(uint8_t volume);
void audio_play(const void *src, uint8_t volume);
void decoder_ops_register(audio_decoder_t *decoder);
void encoder_ops_register(audio_encoder_t *encoder);
esp_err_t audio_init(void);
esp_err_t audio_get_opus_encode_data(uint8_t *out_buf, uint16_t req_len);
void ws_recv_data_handler(const char *data, size_t len);
void opus_decoder_reset(audio_decoder_t *decoder);
size_t mread(char *buf, uint16_t len);
void audio_start_event();
void audio_end_event();

#endif /* __AUDIO_H__ */