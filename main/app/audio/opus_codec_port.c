#include "audio.h"
#include "audio_private.h"
#include "esp_log.h"

#include "ogg.h"
#include "opus.h"

static const char *TAG_OPUS = "opus_codec";

/*--------------------------------------opus_decoder_start-----------------------------------------------*/

typedef struct {
    ogg_sync_state ogsync;
    ogg_stream_state ogstream;
    OpusDecoder *opus_decoder;
    ogg_page current_page;
    ogg_packet current_packet;
    bool stream_inited;
    bool has_found_opus_header;
    bool has_pending_packet;
    uint8_t read_page_cnt;
} opus_decoder_context_t;

static decoder_result_t decoder_init(audio_decoder_t *decoder)
{
    // ESP_LOGI(TAG_OPUS, "[OPUS] Decoder intialization started");
    opus_decoder_context_t *ctx = malloc(sizeof(opus_decoder_context_t));
    if (!ctx) {
        return DECODER_ERROR;
    }
    
    memset(ctx, 0, sizeof(opus_decoder_context_t));
    decoder->context = ctx;
    
    if (ogg_sync_init(&ctx->ogsync) < 0) {
        free(ctx);
        return DECODER_ERROR;
    }
    
    decoder->info.sample_rate = CONFIG_OPUS_AUDIO_SAMPLE_RATE;
    decoder->info.channels = CONFIG_OPUS_AUDIO_CHANNELS;
    
    // ESP_LOGI(TAG_OPUS, "[OPUS] Decoder initialized");
    return DECODER_OK;
}

static decoder_result_t opus_decode_frame(audio_decoder_t *decoder, FILE *file, int16_t *output, uint32_t *samples_decoded)
{
    opus_decoder_context_t *ctx = (opus_decoder_context_t *)decoder->context;
    int err;
    
    while (1) {
        if (ctx->has_pending_packet) {
            goto decode_packet;
        }
        
        if (ctx->stream_inited) {
            int packet_ret = ogg_stream_packetout(&ctx->ogstream, &ctx->current_packet);
            if (packet_ret > 0) {
                ctx->has_pending_packet = true;
                continue;
            } else if (packet_ret < 0) {
                // ESP_LOGE(TAG_OPUS, "[OPUS] Stream packet out error: %d", packet_ret);
                continue;
            }
        }

        int page_ret = ogg_sync_pageout(&ctx->ogsync, &ctx->current_page);
        // log_d("page_ret:%d", page_ret);
        if (page_ret == 1) {
            if (!ctx->stream_inited) {
                if (ogg_stream_init(&ctx->ogstream, ogg_page_serialno(&ctx->current_page)) < 0) {
                    // ESP_LOGE(TAG_OPUS, "[OPUS] Stream init failed");
                    return DECODER_ERROR;
                }
                ctx->stream_inited = true;
                // ESP_LOGI(TAG_OPUS, "[OPUS] Stream initialized, serial: %d", ogg_page_serialno(&ctx->current_page));
            }
            
            if (ogg_stream_pagein(&ctx->ogstream, &ctx->current_page) < 0) {
                // ESP_LOGE(TAG_OPUS, "[OPUS] Page in failed");
                continue;
            }
            continue;
        } else if (page_ret == 0) {
            char *buffer = ogg_sync_buffer(&ctx->ogsync, CONFIG_OPUS_FILE_BUFF_SIZE);
            if (!buffer) {
                // ESP_LOGE(TAG_OPUS, "[OPUS] Sync buffer failed");
                return DECODER_ERROR;
            }
            
            size_t bytes_read = fread(buffer, 1, CONFIG_OPUS_FILE_BUFF_SIZE, file);
            if (bytes_read < 0) {
                // ESP_LOGE(TAG_OPUS, "[OPUS] File read error");
                return DECODER_ERROR;
            }
            // ESP_LOGI(TAG_OPUS, "[OPUS] Read %d bytes from file", (int)bytes_read);
            if (bytes_read == 0) {
                // ESP_LOGI(TAG_OPUS, "[OPUS] End of file");
                return DECODER_EOF;
            }
            
            if (ogg_sync_wrote(&ctx->ogsync, bytes_read) < 0) {
                // ESP_LOGE(TAG_OPUS, "[OPUS] Sync wrote failed");
                return DECODER_ERROR;
            }
            // ESP_LOGI(TAG_OPUS, "[OPUS] Sync wrote %d bytes", (int)bytes_read);
            // LOG_DBG("[OPUS] Read %d bytes from file", (int)bytes_read);
            continue;
        } else {
            // // ESP_LOGE(TAG_OPUS, "[OPUS] Page sync error: %d", page_ret);
            return DECODER_ERROR;
        }
    }

decode_packet:
    ctx->has_pending_packet = false;
    
    if (!ctx->has_found_opus_header) {
        if (ctx->current_packet.bytes >= 8 && memcmp(ctx->current_packet.packet, "OpusHead", 8) == 0) {
            // ESP_LOGI(TAG_OPUS, "[OPUS] Found OpusHead header");
            
            if (ctx->current_packet.bytes >= 10) {
                decoder->info.channels = ctx->current_packet.packet[9];
                // ESP_LOGI(TAG_OPUS, "[OPUS] Channels: %d", decoder->info.channels);
            }
            
            if (ctx->current_packet.bytes >= 16) {
                uint8_t *sr_bytes = ctx->current_packet.packet + 12;
                decoder->info.sample_rate = (uint32_t)sr_bytes[0] | 
                                          (uint32_t)sr_bytes[1] << 8 |
                                          (uint32_t)sr_bytes[2] << 16 | 
                                          (uint32_t)sr_bytes[3] << 24;
                
                // ESP_LOGI(TAG_OPUS, "[OPUS] Sample rate: %ld Hz", decoder->info.sample_rate);
                
                if (decoder->info.sample_rate < 8000 || decoder->info.sample_rate > 48000) {
                    // ESP_LOGE(TAG_OPUS, "[OPUS] Invalid sample rate: %ld", decoder->info.sample_rate);
                    return DECODER_ERROR;
                }
                
                ctx->opus_decoder = opus_decoder_create(decoder->info.sample_rate, decoder->info.channels, &err);
                if (err != OPUS_OK || !ctx->opus_decoder) {
                    // ESP_LOGE(TAG_OPUS, "[OPUS] Decoder create failed: %s", opus_strerror(err));
                    return DECODER_ERROR;
                }
                
                // ESP_LOGI(TAG_OPUS, "[OPUS] Decoder created successfully");
            }
            return DECODER_HEADER_ONLY;
        }
        
        if (ctx->current_packet.bytes >= 8 && memcmp(ctx->current_packet.packet, "OpusTags", 8) == 0) {
            // ESP_LOGI(TAG_OPUS, "[OPUS] Found OpusTags header");
            ctx->has_found_opus_header = true;
            return DECODER_HEADER_ONLY;
        }
        
        // ESP_LOGI(TAG_OPUS, "[OPUS] Skipping unknown header packet");
        return DECODER_HEADER_ONLY;
    }
    
    if (ctx->current_packet.bytes <= 0) {
        // ESP_LOGE(TAG_OPUS, "[OPUS] Empty packet");
        return DECODER_HEADER_ONLY;
    }
    
    opus_int32 output_samples = opus_decode(ctx->opus_decoder, ctx->current_packet.packet, ctx->current_packet.bytes,output, CONFIG_OPUS_FRAME_SAMPLES_MAX, 0);
    
    if (output_samples < 0) {
        // ESP_LOGE(TAG_OPUS, "[OPUS] Decode warning: %s", opus_strerror(output_samples));
        return DECODER_HEADER_ONLY;
    } else if (output_samples == 0) {
        // ESP_LOGE(TAG_OPUS, "[OPUS] Zero samples decoded");
        return DECODER_HEADER_ONLY;
    }

    // if (decoder->info.channels == 1) {
    //     if (output_samples > CONFIG_OPUS_FRAME_SAMPLES_MAX) {
    //         LOG_ERR("[OPUS] Too many samples: %d", output_samples);
    //         return DECODER_ERROR;
    //     }
    //     for (int i = output_samples - 1; i >= 0; i--) {
    //         output[i * 2] = output[i];
    //         output[i * 2 + 1] = output[i];
    //     }
    //     output_samples *= 2;
    // }
    
    *samples_decoded = output_samples;
    // LOG_DBG("[OPUS] Decoded %d samples", output_samples);
    return DECODER_OK;
}

static void decoder_deinit(audio_decoder_t *decoder)
{
    if (decoder->context) {
        opus_decoder_context_t *ctx = (opus_decoder_context_t *)decoder->context;
        if (ctx->opus_decoder) {
            opus_decoder_destroy(ctx->opus_decoder);
        }
        if (ctx->stream_inited) {
            ogg_stream_clear(&ctx->ogstream);
        }
        ogg_sync_clear(&ctx->ogsync);
        free(ctx);
    }
}

/*--------------------------------------opus_decoder_end-----------------------------------------------*/

/*--------------------------------------opus_encoder_start-----------------------------------------------*/

// OPUS 编码上下文（对标解码的 opus_context_t）
typedef struct {
    OpusEncoder *opus_encoder;  // OPUS 编码器实例
    bool encoder_inited;        // 编码器是否初始化完成
} opus_encoder_context_t;

/**
 * @brief 初始化 OPUS 编码器
 * @param encoder 音频编码器实例
 * @return 编码结果
 */
static encoder_result_t encoder_init(audio_encoder_t *encoder)
{
    // ESP_LOGI(TAG_OPUS, "[OPUS] Encoder initialization started");

    // 分配编码上下文内存
    opus_encoder_context_t *ctx = malloc(sizeof(opus_encoder_context_t));
    if (!ctx) {
        ESP_LOGE(TAG_OPUS, "[OPUS] Encoder ctx malloc failed");
        return ENCODER_ERROR;
    }
    memset(ctx, 0, sizeof(opus_encoder_context_t));
    encoder->context = ctx;
    encoder->info.sample_rate = CONFIG_OPUS_AUDIO_SAMPLE_RATE;
    encoder->info.channels = CONFIG_OPUS_AUDIO_CHANNELS;
    encoder->info.bitrate = 16000;
    encoder->info.frame_samples = (encoder->info.sample_rate * 60) / 1000;   // 每帧60ms

    // 验证参数合法性
    if (encoder->info.sample_rate != 8000 && encoder->info.sample_rate != 12000 &&
        encoder->info.sample_rate != 16000 && encoder->info.sample_rate != 24000 && encoder->info.sample_rate != 48000) {
        ESP_LOGE(TAG_OPUS, "[OPUS] Unsupported sample rate: %lu", encoder->info.sample_rate);
        free(ctx);
        return ENCODER_ERROR;
    }
    if (encoder->info.channels < 1 || encoder->info.channels > 2) {
        ESP_LOGE(TAG_OPUS, "[OPUS] Unsupported channels: %u", encoder->info.channels);
        free(ctx);
        return ENCODER_ERROR;
    }

    // 创建 OPUS 编码器实例（语音场景推荐 OPUS_APPLICATION_VOIP）
    int err;
    ctx->opus_encoder = opus_encoder_create(
        encoder->info.sample_rate,
        encoder->info.channels,
        OPUS_APPLICATION_VOIP,  // 语音优先（低延迟、高语音清晰度）
        &err
    );
    if (err != OPUS_OK || !ctx->opus_encoder) {
        ESP_LOGE(TAG_OPUS, "[OPUS] Encoder create failed: %s", opus_strerror(err));
        free(ctx);
        return ENCODER_ERROR;
    }

    // 设置编码器参数
    // 1. 比特率（0 = 自动比特率，推荐手动设置）
    err = opus_encoder_ctl(ctx->opus_encoder, OPUS_SET_BITRATE(encoder->info.bitrate));
    if (err != OPUS_OK) {
        ESP_LOGE(TAG_OPUS, "[OPUS] Set bitrate failed: %s", opus_strerror(err));
    }
    // 2. 复杂度（0-10，越低越快，越高音质越好，默认 9，这里设置为0）
    err = opus_encoder_ctl(ctx->opus_encoder, OPUS_SET_COMPLEXITY(0));
    if (err != OPUS_OK) {
        ESP_LOGE(TAG_OPUS, "[OPUS] Set complexity failed: %s", opus_strerror(err));
    }

    ctx->encoder_inited = true;
    // ESP_LOGI(TAG_OPUS, "[OPUS] Encoder initialized (sr:%d, ch:%d, bitrate:%d, frame_samples:%d)",
    //          ctx->sample_rate, ctx->channels, ctx->bitrate, ctx->frame_samples);
    return ENCODER_OK;
}

/**
 * @brief 编码一帧 PCM 数据为 OPUS 数据包
 * @param encoder 音频编码器实例
 * @param input 输入 PCM 数据（int16_t 格式）
 * @param input_samples 输入采样数（单通道总采样数，立体声需 *2）
 * @param output 输出 OPUS 编码数据缓冲区
 * @param output_buf_size 输出缓冲区大小（字节）
 * @param bytes_encoded 输出：实际编码后的字节数
 * @return 编码结果
 */
static encoder_result_t opus_encode_frame(audio_encoder_t *encoder,
                                         const int16_t *input,
                                         uint32_t input_samples,
                                         uint8_t *output,
                                         uint32_t output_buf_size,
                                         uint32_t *bytes_encoded)
{
    opus_encoder_context_t *ctx = (opus_encoder_context_t *)encoder->context;
    *bytes_encoded = 0;

    // 检查编码器状态
    if (!ctx || !ctx->encoder_inited || !ctx->opus_encoder) {
        ESP_LOGE(TAG_OPUS, "[OPUS] Encoder not initialized");
        return ENCODER_ERROR;
    }

    // 检查输入数据
    if (!input || input_samples == 0) {
        ESP_LOGE(TAG_OPUS, "[OPUS] Empty input samples");
        return ENCODER_EMPTY;
    }

    // 检查输入采样数是否匹配帧大小
    if (input_samples != encoder->info.frame_samples) {
        ESP_LOGE(TAG_OPUS, "[OPUS] Input samples mismatch (expect:%lu, got:%lu)",
                 encoder->info.frame_samples, input_samples);
        return ENCODER_ERROR;
    }

    // 执行 OPUS 编码
    int encode_ret = opus_encode(
        ctx->opus_encoder,
        input,                           // 输入 PCM 数据（int16_t）
        encoder->info.frame_samples,     // 每帧采样数（单通道）
        output,                          // 输出编码数据
        output_buf_size                  // 输出缓冲区大小
    );

    // 检查输出缓冲区大小（OPUS 单帧最大编码后字节数约 1500）
    if (output_buf_size < encode_ret) {  // 预留足够缓冲区
        ESP_LOGE(TAG_OPUS, "[OPUS] Output buffer too small (need >=%d, got:%lu)", encode_ret, output_buf_size);
        return ENCODER_TOO_BIG;
    }

    // 处理编码结果
    if (encode_ret < 0) {
        ESP_LOGE(TAG_OPUS, "[OPUS] Encode failed: %s", opus_strerror(encode_ret));
        return ENCODER_ERROR;
    } else if (encode_ret == 0) {
        ESP_LOGW(TAG_OPUS, "[OPUS] Encode zero bytes (silence?)");
        *bytes_encoded = 0;
        return ENCODER_OK;
    }

    *bytes_encoded = encode_ret;
    ESP_LOGI(TAG_OPUS, "[OPUS] Encoded %lu samples -> %d bytes", input_samples, encode_ret);
    return ENCODER_OK;
}

/**
 * @brief 反初始化 OPUS 编码器
 * @param encoder 音频编码器实例
 */
static void encoder_deinit(audio_encoder_t *encoder)
{
    if (encoder->context) {
        opus_encoder_context_t *ctx = (opus_encoder_context_t *)encoder->context;
        // 销毁编码器实例
        if (ctx->opus_encoder) {
            opus_encoder_destroy(ctx->opus_encoder);
        }
        // 释放上下文内存
        free(ctx);
        encoder->context = NULL;
    }
    // ESP_LOGI(TAG_OPUS, "[OPUS] Encoder deinitialized");
}



/*--------------------------------------opus_encoder_end-----------------------------------------------*/

void decoder_ops_register(audio_decoder_t *decoder)
{
    decoder->init = decoder_init;
    decoder->decode_frame = opus_decode_frame;
    decoder->deinit = decoder_deinit;
    // // ESP_LOGI(TAG_OPUS, "[OPUS] Decoder operations registered successfully");
}

void encoder_ops_register(audio_encoder_t *encoder)
{
    encoder->init = encoder_init;
    encoder->encode_frame = opus_encode_frame;
    encoder->deinit = encoder_deinit;
    // // ESP_LOGI(TAG_OPUS, "[OPUS] Encoder operations registered successfully");
}
