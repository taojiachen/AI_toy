#ifndef __AUDIO_H__
#define __AUDIO_H__

#include "stdint.h"
#include <stdbool.h>
#include "audio_private.h"

// 公共函数声明
void audio_set_volume(uint8_t volume);
void audio_play(const void *src, uint8_t volume);
void decoder_ops_register(audio_decoder_t *decoder);
void audio_init(void);

#endif /* __AUDIO_H__ */