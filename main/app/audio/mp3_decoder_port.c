#ifdef Helix_mp3
#include "audio.h"
#include "audio_private.h"
#include "esp_log.h"
#include "esp_heap_caps.h" // 用于指定内存类型

#include "mp3dec.h"

static const char *TAG = "MP3Decoder";

// MP3固定参数（建议放头文件）
#define MP3_MAX_FRAME_BYTES 1440  // MP3单帧最大字节数
#define MP3_MAX_OUTPUT_SAMPLES 2304 // 单帧最大输出采样数（1152*2）
#define ID3V2_HEADER_SIZE 10      // ID3v2头部固定10字节

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

uint32_t mp3_get_id3v2_size(const uint8_t *buf, size_t buf_len)
{
    if (buf == NULL || buf_len < ID3V2_HEADER_SIZE) {
        return 0; // 缓冲区不足，无ID3v2
    }

    uint32_t id3v2_size = 0;
    // ID3v2标识：前3字节为"ID3"，版本号≥2
    if ((buf[0] == 'I') && (buf[1] == 'D') && (buf[2] == '3') && (buf[3] < 0xFF) && (buf[4] < 0xFF)) {
        // ID3v2大小计算：第6-9字节是同步安全整数（最高位为0）
        id3v2_size = ((uint32_t)buf[6] << 21) | ((uint32_t)buf[7] << 14) | ((uint32_t)buf[8] << 7) | buf[9];
        id3v2_size += ID3V2_HEADER_SIZE; // 包含头部本身
    }
    return id3v2_size;
}

static decoder_result_t mp3_init(audio_decoder_t *decoder)
{
    if (decoder == NULL) return DECODER_ERROR;

    mp3_context_t *ctx = heap_caps_malloc(sizeof(mp3_context_t), MALLOC_CAP_INTERNAL); // 强制内部RAM
    if (!ctx) return DECODER_ERROR;
    
    memset(ctx, 0, sizeof(mp3_context_t));
    decoder->context = ctx;
    
    ctx->mp3_decoder = MP3InitDecoder();
    if (!ctx->mp3_decoder) {
        heap_caps_free(ctx);
        return DECODER_ERROR;
    }
    
    ctx->buffer_size = CONFIG_MP3_FILE_BUFF_SIZE;
    // 强制分配内部RAM，避免PSRAM访问问题
    ctx->input_buffer = heap_caps_malloc(ctx->buffer_size, MALLOC_CAP_INTERNAL);
    if (!ctx->input_buffer) {
        MP3FreeDecoder(ctx->mp3_decoder);
        heap_caps_free(ctx);
        return DECODER_ERROR;
    }
    
    ctx->first_read = true;
    // 初始值仅为占位，解码时会覆盖为实际值
    decoder->info.sample_rate = 0;
    decoder->info.channels = 0;
    
    ESP_LOGI(TAG, "Decoder initialized");
    return DECODER_OK;
}

static decoder_result_t mp3_decode_frame(audio_decoder_t *decoder, FILE *file, int16_t *output, uint32_t *samples_decoded)
{
    // 1. 校验输入参数（防御性编程）
    if (decoder == NULL || file == NULL || output == NULL || samples_decoded == NULL) {
        ESP_LOGE(TAG, "Invalid input parameters");
        return DECODER_ERROR;
    }

    mp3_context_t *ctx = (mp3_context_t *)decoder->context;
    if (ctx == NULL) return DECODER_ERROR;

    // 2. 首次读取：处理ID3v2头部
    if (ctx->first_read) {
        fseek(file, 0, SEEK_SET);
        size_t br = fread(ctx->input_buffer, 1, ctx->buffer_size, file);
        if (br == 0) {
            return feof(file) ? DECODER_EOF : DECODER_ERROR;
        }
        
        // 安全解析ID3v2（检查缓冲区长度）
        ctx->id3v2_size = mp3_get_id3v2_size(ctx->input_buffer, br);
        ESP_LOGI(TAG, "ID3v2 size: %ld", ctx->id3v2_size);
        
        // 跳转到ID3v2之后的位置（检查fseek是否成功）
        if (fseek(file, ctx->id3v2_size, SEEK_SET) != 0) {
            ESP_LOGE(TAG, "Failed to skip ID3v2 header");
            return DECODER_ERROR;
        }
        
        // 读取实际音频数据
        br = fread(ctx->input_buffer, 1, ctx->buffer_size, file);
        if (br == 0) {
            return feof(file) ? DECODER_EOF : DECODER_ERROR;
        }
        
        ctx->bytes_left = br;
        ctx->read_ptr = ctx->input_buffer;
        ctx->first_read = false;
    }

    // 3. 确保缓冲区有足够数据（至少MAX_FRAME_BYTES）
    while (ctx->bytes_left < MP3_MAX_FRAME_BYTES) {
        // 移动剩余数据到缓冲区开头
        memmove(ctx->input_buffer, ctx->read_ptr, ctx->bytes_left);
        uint32_t fill_size = ctx->buffer_size - ctx->bytes_left;
        size_t br = fread(ctx->input_buffer + ctx->bytes_left, 1, fill_size, file);
        
        if (br == 0) {
            return feof(file) ? DECODER_EOF : DECODER_ERROR;
        }
        
        ctx->bytes_left += br;
        ctx->read_ptr = ctx->input_buffer;

        // 若填充后仍不足，说明文件已读完且数据不完整
        if (ctx->bytes_left < MP3_MAX_FRAME_BYTES) {
            ESP_LOGW(TAG, "Insufficient data for MP3 frame");
            return DECODER_EOF;
        }
    }

    // 4. 查找MP3同步字（循环重试，直到找到或EOF）
    int offset = -1;
    int retry = 3; // 最多重试3次（跳过无效数据）
    while (offset == -1 && retry-- > 0) {
        offset = MP3FindSyncWord(ctx->read_ptr, ctx->bytes_left);
        if (offset == -1) {
            // 跳过1字节无效数据，继续查找
            ctx->read_ptr += 1;
            ctx->bytes_left -= 1;
            // 若剩余数据不足，填充新数据
            if (ctx->bytes_left < MP3_MAX_FRAME_BYTES) {
                memmove(ctx->input_buffer, ctx->read_ptr, ctx->bytes_left);
                size_t br = fread(ctx->input_buffer + ctx->bytes_left, 1, ctx->buffer_size - ctx->bytes_left, file);
                if (br == 0) return feof(file) ? DECODER_EOF : DECODER_ERROR;
                ctx->bytes_left += br;
                ctx->read_ptr = ctx->input_buffer;
            }
        }
    }
    if (offset == -1) {
        ESP_LOGE(TAG, "No MP3 sync word found");
        return DECODER_EOF;
    }

    // 5. 定位到同步字位置
    ctx->read_ptr += offset;
    ctx->bytes_left -= offset;

    // 6. 解码帧（确保output缓冲区足够）
    int ret = MP3Decode(ctx->mp3_decoder, &ctx->read_ptr, &ctx->bytes_left, output, 0);
    if (ret != ERR_MP3_NONE) {
        ESP_LOGE(TAG, "Decode error: %d", ret);
        return DECODER_ERROR;
    }

    // 7. 获取帧信息并校验
    MP3GetLastFrameInfo(ctx->mp3_decoder, &ctx->mp3_frame_info);
    uint32_t pcm_samples = ctx->mp3_frame_info.outputSamps;
    if (pcm_samples == 0 || pcm_samples > MP3_MAX_OUTPUT_SAMPLES) {
        ESP_LOGE(TAG, "Invalid output samples: %ld", pcm_samples);
        return DECODER_ERROR;
    }

    // 8. 更新解码器信息
    decoder->info.sample_rate = ctx->mp3_frame_info.samprate;
    decoder->info.channels = ctx->mp3_frame_info.nChans;

    *samples_decoded = pcm_samples;
    // ESP_LOGD(TAG, "Decoded %d samples, SR=%d, CH=%d", pcm_samples, decoder->info.sample_rate, decoder->info.channels);
    return DECODER_OK;
}

static void mp3_deinit(audio_decoder_t *decoder)
{
    if (decoder == NULL || decoder->context == NULL) return;

    mp3_context_t *ctx = (mp3_context_t *)decoder->context;
    if (ctx->mp3_decoder) {
        MP3FreeDecoder(ctx->mp3_decoder);
    }
    if (ctx->input_buffer) {
        heap_caps_free(ctx->input_buffer); // 对应heap_caps_malloc
    }
    heap_caps_free(ctx);
    decoder->context = NULL; // 避免野指针
}

void decoder_ops_register (audio_decoder_t *decoder)
{
    if (decoder == NULL) return;

    decoder->init = mp3_init;
    decoder->decode_frame = mp3_decode_frame;
    decoder->deinit = mp3_deinit;
    ESP_LOGI(TAG, "Decoder operations registered successfully");
}

#endif