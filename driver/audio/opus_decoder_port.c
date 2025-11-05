#include "audio.h"
#include "audio_private.h"

#include "ogg.h"
#include "opus.h"

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
} opus_context_t;

static decoder_result_t opus_init(audio_decoder_t *decoder)
{
    opus_context_t *ctx = osal_malloc(sizeof(opus_context_t));
    if (!ctx) {
        return DECODER_ERROR;
    }
    
    memset(ctx, 0, sizeof(opus_context_t));
    decoder->context = ctx;
    
    if (ogg_sync_init(&ctx->ogsync) < 0) {
        osal_free(ctx);
        return DECODER_ERROR;
    }
    
    decoder->info.sample_rate = CONFIG_OPUS_AUDIO_SAMPLE_RATE;
    decoder->info.channels = CONFIG_OPUS_AUDIO_CHANNELS;
    
    LOG_INF("[OPUS] Decoder initialized");
    return DECODER_OK;
}

static decoder_result_t opus_decode_frame(audio_decoder_t *decoder, struct fs_file_t *file, int16_t *output, uint32_t *samples_decoded)
{
    opus_context_t *ctx = (opus_context_t *)decoder->context;
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
                LOG_WRN("[OPUS] Stream packet out error: %d", packet_ret);
                continue;
            }
        }

        int page_ret = ogg_sync_pageout(&ctx->ogsync, &ctx->current_page);
        log_d("page_ret:%d", page_ret);
        if (page_ret == 1) {
            if (!ctx->stream_inited) {
                if (ogg_stream_init(&ctx->ogstream, ogg_page_serialno(&ctx->current_page)) < 0) {
                    LOG_ERR("[OPUS] Stream init failed");
                    return DECODER_ERROR;
                }
                ctx->stream_inited = true;
                LOG_INF("[OPUS] Stream initialized, serial: %d", ogg_page_serialno(&ctx->current_page));
            }
            
            if (ogg_stream_pagein(&ctx->ogstream, &ctx->current_page) < 0) {
                LOG_ERR("[OPUS] Page in failed");
                continue;
            }
            continue;
        } else if (page_ret == 0) {
            char *buffer = ogg_sync_buffer(&ctx->ogsync, CONFIG_OPUS_FILE_BUFF_SIZE);
            if (!buffer) {
                LOG_ERR("[OPUS] Sync buffer failed");
                return DECODER_ERROR;
            }
            
            os_ssize_t bytes_read = fs_read(file, buffer, CONFIG_OPUS_FILE_BUFF_SIZE);
            if (bytes_read < 0) {
                LOG_ERR("[OPUS] File read error");
                return DECODER_ERROR;
            }
            
            if (bytes_read == 0) {
                LOG_INF("[OPUS] End of file");
                return DECODER_EOF;
            }
            
            if (ogg_sync_wrote(&ctx->ogsync, bytes_read) < 0) {
                LOG_ERR("[OPUS] Sync wrote failed");
                return DECODER_ERROR;
            }
            
            // LOG_DBG("[OPUS] Read %d bytes from file", (int)bytes_read);
            continue;
        } else {
            LOG_ERR("[OPUS] Page sync error: %d", page_ret);
            return DECODER_ERROR;
        }
    }

decode_packet:
    ctx->has_pending_packet = false;
    
    if (!ctx->has_found_opus_header) {
        if (ctx->current_packet.bytes >= 8 && memcmp(ctx->current_packet.packet, "OpusHead", 8) == 0) {
            LOG_INF("[OPUS] Found OpusHead header");
            
            if (ctx->current_packet.bytes >= 10) {
                decoder->info.channels = ctx->current_packet.packet[9];
                LOG_INF("[OPUS] Channels: %d", decoder->info.channels);
            }
            
            if (ctx->current_packet.bytes >= 16) {
                uint8_t *sr_bytes = ctx->current_packet.packet + 12;
                decoder->info.sample_rate = (uint32_t)sr_bytes[0] | 
                                          (uint32_t)sr_bytes[1] << 8 |
                                          (uint32_t)sr_bytes[2] << 16 | 
                                          (uint32_t)sr_bytes[3] << 24;
                
                LOG_INF("[OPUS] Sample rate: %d Hz", decoder->info.sample_rate);
                
                if (decoder->info.sample_rate < 8000 || decoder->info.sample_rate > 48000) {
                    LOG_ERR("[OPUS] Invalid sample rate: %d", decoder->info.sample_rate);
                    return DECODER_ERROR;
                }
                
                ctx->opus_decoder = opus_decoder_create(decoder->info.sample_rate, decoder->info.channels, &err);
                if (err != OPUS_OK || !ctx->opus_decoder) {
                    LOG_ERR("[OPUS] Decoder create failed: %s", opus_strerror(err));
                    return DECODER_ERROR;
                }
                
                LOG_INF("[OPUS] Decoder created successfully");
            }
            return DECODER_HEADER_ONLY;
        }
        
        if (ctx->current_packet.bytes >= 8 && memcmp(ctx->current_packet.packet, "OpusTags", 8) == 0) {
            LOG_INF("[OPUS] Found OpusTags header");
            ctx->has_found_opus_header = true;
            return DECODER_HEADER_ONLY;
        }
        
        LOG_DBG("[OPUS] Skipping unknown header packet");
        return DECODER_HEADER_ONLY;
    }
    
    if (ctx->current_packet.bytes <= 0) {
        LOG_WRN("[OPUS] Empty packet");
        return DECODER_HEADER_ONLY;
    }
    
    opus_int32 output_samples = opus_decode(ctx->opus_decoder, ctx->current_packet.packet, ctx->current_packet.bytes,output, CONFIG_OPUS_FRAME_SAMPLES_MAX, 0);
    
    if (output_samples < 0) {
        LOG_WRN("[OPUS] Decode warning: %s", opus_strerror(output_samples));
        return DECODER_HEADER_ONLY;
    } else if (output_samples == 0) {
        LOG_WRN("[OPUS] Zero samples decoded");
        return DECODER_HEADER_ONLY;
    }

    if (decoder->info.channels == 1) {
        if (output_samples > CONFIG_OPUS_FRAME_SAMPLES_MAX) {
            LOG_ERR("[OPUS] Too many samples: %d", output_samples);
            return DECODER_ERROR;
        }
        for (int i = output_samples - 1; i >= 0; i--) {
            output[i * 2] = output[i];
            output[i * 2 + 1] = output[i];
        }
        output_samples *= 2;
    }
    
    *samples_decoded = output_samples;
    // LOG_DBG("[OPUS] Decoded %d samples", output_samples);
    return DECODER_OK;
}

static void opus_deinit(audio_decoder_t *decoder)
{
    if (decoder->context) {
        opus_context_t *ctx = (opus_context_t *)decoder->context;
        if (ctx->opus_decoder) {
            opus_decoder_destroy(ctx->opus_decoder);
        }
        if (ctx->stream_inited) {
            ogg_stream_clear(&ctx->ogstream);
        }
        ogg_sync_clear(&ctx->ogsync);
        osal_free(ctx);
    }
}

void decoder_ops_register(audio_decoder_t *decoder)
{
    decoder->init = opus_init;
    decoder->decode_frame = opus_decode_frame;
    decoder->deinit = opus_deinit;
    LOG_INF("[OPUS] Decoder operations registered successfully");
}