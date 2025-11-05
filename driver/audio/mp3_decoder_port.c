#include "audio.h"
#include "audio_private.h"

#include "mp3dec.h"

typedef struct {
    HMP3Decoder mp3_decoder;
    MP3FrameInfo mp3_frame_info;
    uint8_t *input_buffer;
    uint32_t buffer_size;
    uint32_t bytes_in_buffer;
    uint8_t *read_ptr;
    int bytes_left;
    uint32_t id3v2_size;
    bool first_read;
} mp3_context_t;

uint32_t mp3_get_id3v2_size(const uint8_t *buf)
{
    if (buf == NULL) {
        return 0;
    }

    uint32_t id3v2_size = 0;
    if ((buf[0] == 'I') && (buf[1] == 'D') && (buf[2] == '3')) {
        id3v2_size = (buf[6] << 21) | (buf[7] << 14) | (buf[8] << 7) | buf[9];
    }
    return id3v2_size;
}

static decoder_result_t mp3_init(audio_decoder_t *decoder)
{
    mp3_context_t *ctx = osal_malloc(sizeof(mp3_context_t));
    if (!ctx) return DECODER_ERROR;
    
    memset(ctx, 0, sizeof(mp3_context_t));
    decoder->context = ctx;
    
    ctx->mp3_decoder = MP3InitDecoder();
    if (!ctx->mp3_decoder) {
        osal_free(ctx);
        return DECODER_ERROR;
    }
    
    ctx->buffer_size = CONFIG_MP3_FILE_BUFF_SIZE;
    ctx->input_buffer = osal_malloc(ctx->buffer_size);
    if (!ctx->input_buffer) {
        MP3FreeDecoder(ctx->mp3_decoder);
        osal_free(ctx);
        return DECODER_ERROR;
    }
    
    ctx->first_read = true;
    decoder->info.sample_rate = CONFIG_MP3_AUDIO_SAMPLE_RATE;
    decoder->info.channels = CONFIG_MP3_AUDIO_CHANNELS;
    
    LOG_INF("[MP3] Decoder initialized");
    return DECODER_OK;
}

static decoder_result_t mp3_decode_frame(audio_decoder_t *decoder, struct fs_file_t *file, int16_t *output, uint32_t *samples_decoded)
{
    mp3_context_t *ctx = (mp3_context_t *)decoder->context;
    
    if (ctx->first_read) {
        fs_seek(file, 0, FS_SEEK_SET);
        os_ssize_t br = fs_read(file, ctx->input_buffer, ctx->buffer_size);
        if (br <= 0) return DECODER_EOF;
        
        ctx->id3v2_size = mp3_get_id3v2_size(ctx->input_buffer);
        LOG_INF("[MP3] ID3v2 size: %d", ctx->id3v2_size);
        
        fs_seek(file, ctx->id3v2_size, FS_SEEK_SET);
        br = fs_read(file, ctx->input_buffer, ctx->buffer_size);
        if (br <= 0) return DECODER_EOF;
        
        ctx->bytes_left = br;
        ctx->read_ptr = ctx->input_buffer;
        ctx->first_read = false;
    }
    
    if (ctx->bytes_left < CONFIG_MP3_MAX_FRAME_BYTES) {
        memmove(ctx->input_buffer, ctx->read_ptr, ctx->bytes_left);
        uint32_t fill_size = ctx->buffer_size - ctx->bytes_left;
        os_ssize_t br = fs_read(file, ctx->input_buffer + ctx->bytes_left, fill_size);
        
        if (br < 0) return DECODER_ERROR;
        
        if (br < fill_size) {
            memset(ctx->input_buffer + ctx->bytes_left + br, 0, fill_size - br);
        }
        
        if (br == 0 && ctx->bytes_left == 0) {
            return DECODER_EOF;
        }
        
        ctx->bytes_left = ctx->bytes_left + br;
        ctx->read_ptr = ctx->input_buffer;
    }
    
    int offset = MP3FindSyncWord(ctx->read_ptr, ctx->bytes_left);
    if (offset == -1) {
        return DECODER_EOF;
    }
    
    ctx->read_ptr += offset;
    ctx->bytes_left -= offset;
    
    int ret = MP3Decode(ctx->mp3_decoder, &ctx->read_ptr, &ctx->bytes_left, output, 0);
    if (ret != ERR_MP3_NONE) {
        LOG_ERR("[MP3] Decode error: %d", ret);
        return DECODER_ERROR;
    }
    
    MP3GetLastFrameInfo(ctx->mp3_decoder, &ctx->mp3_frame_info);
    uint32_t pcm_samples = ctx->mp3_frame_info.outputSamps;
    
    decoder->info.sample_rate = ctx->mp3_frame_info.samprate;
    decoder->info.channels = ctx->mp3_frame_info.nChans;
    
    if (ctx->mp3_frame_info.nChans == 1) {
        if (pcm_samples > 0 && pcm_samples <= 576) {
            for (int i = pcm_samples - 1; i >= 0; i--) {
                output[i * 2] = output[i];
                output[i * 2 + 1] = output[i];
            }
            pcm_samples *= 2;
        }
    }
    
    *samples_decoded = pcm_samples;
    // LOG_DBG("[MP3] Decoded %d samples, SR=%d, CH=%d", pcm_samples, decoder->info.sample_rate, decoder->info.channels);
    return DECODER_OK;
}

static void mp3_deinit(audio_decoder_t *decoder)
{
    if (decoder->context) {
        mp3_context_t *ctx = (mp3_context_t *)decoder->context;
        if (ctx->mp3_decoder) {
            MP3FreeDecoder(ctx->mp3_decoder);
        }
        if (ctx->input_buffer) {
            osal_free(ctx->input_buffer);
        }
        osal_free(ctx);
    }
}

void decoder_ops_register (audio_decoder_t *decoder)
{
    decoder->init = mp3_init;
    decoder->decode_frame = mp3_decode_frame;
    decoder->deinit = mp3_deinit;
    LOG_INF("[MP3] Decoder operations registered successfully");
}