#include "audio.h"
#include "audio_private.h"
#include "i2s.h"

#define TX_SEM_TIMEOUT 500

decoder_info_t aud_player;

__attribute__((weak)) void decoder_ops_register(audio_decoder_t *decoder)
{
    LOG_ERR("decoder_ops_register: No strong implementation found!");
}

static audio_decoder_t* audio_decoder_register()
{
    audio_decoder_t *decoder = osal_malloc(sizeof(audio_decoder_t));
    if (!decoder) return NULL;

    memset(decoder, 0, sizeof(audio_decoder_t));

    decoder_ops_register(decoder);

    return decoder;
}

void audio_decoder_deinit(audio_decoder_t *decoder)
{
    if (decoder) {
        if (decoder->deinit) {
            decoder->deinit(decoder);
        }
        osal_free(decoder);
    }
}

static void aud_player_file(device_audio_t *dev, const char *path)
{
    device_t *i2s_bus = dev->info.i2s_bus;
    struct i2s_device *i2s = (struct i2s_device *)i2s_bus;
    i2s_cbuff_t *cbuff = &i2s->tx_cbuff;

    audio_decoder_t *decoder = NULL;
    struct fs_file_t filep;
    osal_status_t stat;
    i2s_frame_t *tf;

#if (CONFIG_USING_I2S_BCK_CONTINUOUS_OUTPUT == 0)
    device_control(i2s_bus, I2S_CMD_TX_ENABLE, NULL);
#endif

    reset_i2s_cbuff(cbuff);
    dev->is_file_end = false;
    dev->is_transimitting = false;

    decoder = audio_decoder_register();
    if (!decoder->init || !decoder->decode_frame || !decoder->deinit) {
        LOG_ERR("[aud] Failed to create decoder for %s\n", path);
        return;
    }

    fs_file_t_init(&filep);
    int ret = fs_open(&filep, path, FS_O_READ);
    if (ret) {
        LOG_ERR("[aud] fs_open failed: %d\n", ret);
        audio_decoder_deinit(decoder);
        fs_close(&filep);
        return;
    }
    LOG_ERR("[aud] fs_open success\n");
    if (decoder->init(decoder) != DECODER_OK) {
        LOG_ERR("[aud] Decoder init failed\n");
        goto EXIT;
    }

    LOG_INF("[aud] Starting decode loop\n");

    while (!dev->is_file_end) {
        if (osal_sema_acquire(dev->sem_stop, 1) == S_OS_OK) {
            LOG_INF("[aud] audio sem_stop\n");
            goto EXIT;
        }

        if (is_i2s_cbuff_full(cbuff)) {
            if (!dev->is_transimitting) {
                osal_sema_acquire(i2s_bus->tx_sem, 0);
                i2s->ops->transimit(i2s, (uint8_t *)(cbuff->frame[0].data),
                cbuff->frame[0].valid_size);
                dev->is_transimitting = true;

#if (CONFIG_USING_I2S_BCK_CONTINUOUS_OUTPUT)
                cbuff->is_audio_data = true;
#else
                device_control((device_t *)dev, AUDIO_CMD_MUTE_DISABLE, NULL);
#endif
            }

            stat = osal_sema_acquire(i2s_bus->tx_sem, TX_SEM_TIMEOUT);
            if (stat != S_OS_OK) {
                LOG_ERR("[aud] i2s_bus->tx_sem error %d\n", stat);
                if (stat == S_OS_ERROR_TIMEOUT) {
                    i2s->ops->transimit(i2s, (uint8_t *)(cbuff->frame[cbuff->head].data), cbuff->frame[cbuff->head].valid_size);
                } else {
                    goto EXIT;
                }
            }
        }

        tf = &cbuff->frame[cbuff->tail];
        uint32_t samples_decoded = 0;
        decoder_result_t decode_result = decoder->decode_frame(decoder, &filep, tf->data, &samples_decoded);

        switch (decode_result) {

            case DECODER_OK:
                if (aud_player.sample_rate != decoder->info.sample_rate) {
                    aud_player.sample_rate = decoder->info.sample_rate;
                    i2s->ops->set_frq(i2s, aud_player.sample_rate);
                    LOG_INF("[aud] I2S updated to %d Hz\n", aud_player.sample_rate);
                }
                tf->valid_size = samples_decoded;
                int sr = osal_kernel_irq_disable();
                cbuff->tail = (cbuff->tail + 1) % cbuff->size;
                cbuff->count++;
                osal_kernel_irq_enable(sr);
                break;

            case DECODER_HEADER_ONLY:
                // 头部解析或跳过，继续下一次循环
                break;

            case DECODER_EOF:
                dev->is_file_end = true;
                LOG_INF("[aud] Decoder EOF\n");
                break;

            case DECODER_ERROR:
            default:
                LOG_ERR("[aud] Decode error: %d\n", decode_result);
                goto EXIT;
        }
    }

    while (!is_i2s_cbuff_empty(cbuff)) {
        if (osal_sema_acquire(dev->sem_stop, 1) == S_OS_OK) {
            LOG_INF("[aud] audio sem_stop\n");
            goto EXIT;
        }

        stat = osal_sema_acquire(i2s_bus->tx_sem, TX_SEM_TIMEOUT);
        if (stat != S_OS_OK) {
            LOG_ERR("[aud] wait DMA tx all buffer data error %d\n", stat);
            i2s->ops->transimit(i2s, (uint8_t *)(cbuff->frame[cbuff->head].data), cbuff->frame[cbuff->head].valid_size);
        }
    }

#if CONFIG_DISABLE_I2S_TX_WHEN_NO_DATA
     device_control(i2s_bus, I2S_CMD_TX_DISABLE, NULL);
#endif

    if (dev->eof_cb) {
        dev->eof_cb();
    }

EXIT:
#if (CONFIG_USING_I2S_BCK_CONTINUOUS_OUTPUT)
      cbuff->is_audio_data = false;
#else
     device_control((device_t *)dev, AUDIO_CMD_MUTE_ENABLE, NULL);
#endif
    fs_close(&filep);
    audio_decoder_deinit(decoder);
}

static void audio_set_volume(device_t *dev, uint8_t volume)
{
    if (dev) {
        os_err_t ret = device_control(dev, AUDIO_CMD_SET_VOLUME, &volume);
        log_i("[aud] audio_play set volume success? %d, volume: %d", ret, volume);
    }
}

void audio_play(device_t *dev, const void *src, uint8_t volume)
{
    device_audio_t *device_audio = (device_audio_t *)dev;

    // 初始化新播放状态
    device_audio->is_playing = true;
    device_audio->is_file_end = false;

    // LOG_INF("[aud] audio source type: %d\n", src_type);
    audio_set_volume(dev, volume);

    if (device_audio->sem_stop) {
        osal_sema_acquire(device_audio->sem_stop, 0); // 清除停止信号量
    }

    aud_player_file(device_audio, (const char *)src);

    device_audio->is_playing = false;
}

static void set_speaker_enable_pin(device_audio_t *device, bool is_on)
{
    if (!device) {
        return;
    }

    struct device_pin_status pin_status;
    pin_status.pin = device->info.spk_pin_get.pin;
    pin_status.status = is_on ? PIN_HIGH : PIN_LOW;
    device_write_block(device->info.spk_en_pin, 0, &pin_status, sizeof(struct device_pin_status));
}

static int audio_speaker_enable_pin_init(device_audio_t *device)
{
    if (!device) {
        return -1;
    }

    os_err_t result = OS_SUCCESS;
    struct device_pin_mode pin_mode;

    if (device->info.spk_pin_get.name != NULL) {
        device->info.spk_en_pin = device_find("pin_0");
        if (!device->info.spk_en_pin) {
            LOG_ERR("[aud] spk_en_pin device not found\n");
            return -1;
        }

        device_open(device->info.spk_en_pin);

        result = device_control(device->info.spk_en_pin, PIN_CMD_GET,
                                &device->info.spk_pin_get);
        LOG_DBG("[aud] speaker enable pin get: %d, %s, %d\n", result,
                device->info.spk_pin_get.name, device->info.spk_pin_get.pin);

        pin_mode.pin = device->info.spk_pin_get.pin;
        pin_mode.mode = PIN_MODE_OUTPUT;
        result = device_control(device->info.spk_en_pin, PIN_CMD_MODE, &pin_mode);
        LOG_DBG("[aud] speaker enable pin mode: %d, %d, %d\n", result,
                pin_mode.mode, pin_mode.pin);

        set_speaker_enable_pin(device, true);
    }

    return result;
}

static int audio_reset_pin_init(device_audio_t *device)
{
    if (!device) {
        return -1;
    }

    os_err_t result = OS_SUCCESS;
    struct device_pin_mode pin_mode;

    if (device->info.rst_pin_get.name != NULL) {
        device->info.rst_pin = device_find("pin_0");
        if (!device->info.rst_pin) {
            LOG_ERR("[aud] rst_pin device not found\n");
            return -1;
        }

        device_open(device->info.rst_pin);

        result = device_control(device->info.rst_pin, PIN_CMD_GET, &device->info.rst_pin_get);
        LOG_DBG("[aud] reset pin get: %d, %s, %d\n", result, device->info.rst_pin_get.name, device->info.rst_pin_get.pin);

        pin_mode.pin = device->info.rst_pin_get.pin;
        pin_mode.mode = PIN_MODE_OUTPUT;
        result = device_control(device->info.rst_pin, PIN_CMD_MODE, &pin_mode);
        LOG_DBG("[aud] reset pin mode: %d, %d, %d\n", result, pin_mode.mode, pin_mode.pin);
    }

    return result;
}

static int audio_i2c_init(device_audio_t *device)
{
    if (!device) {
        return -1;
    }

    os_err_t result;
    device_t *i2c_bus = OS_NULL;

    i2c_bus = device_find(device->info.i2c_cfg.bus_name);
    if (!i2c_bus) {
        LOG_ERR("[aud] i2c bus not found\n");
        return -1;
    }

    result = device_open(i2c_bus);
    if (result != OS_SUCCESS) {
        LOG_ERR("[aud] i2c bus open failed\n");
        return -1;
    }

    device->info.i2c_bus = dev_i2c_device_register(&device->info.i2c_cfg, "audio_i2c", i2c_bus);
    if (!device->info.i2c_bus) {
        LOG_ERR("[aud] i2c device register failed\n");
        return -1;
    }

    result = device_open(device->info.i2c_bus);
    if (result != OS_SUCCESS) {
        LOG_ERR("[aud] audio i2c device open failed\n");
        return -1;
    }

    LOG_INF("[aud] i2c init success\n");
    return 0;
}

static int audio_i2s_init(device_audio_t *device)
{
    if (!device) {
        return -1;
    }

    device_t *i2s_bus;

    i2s_bus = device_find(device->info.i2s_bus_name);
    if (!i2s_bus) {
        LOG_ERR("[aud] i2s bus not found\n");
        return -1;
    }

    os_err_t result = device_open(i2s_bus);
    if (result != OS_SUCCESS) {
        LOG_ERR("[aud] i2s bus open failed\n");
        return -1;
    }

    device->info.i2s_bus = i2s_bus;

    LOG_INF("[aud] i2s init success\n");
    return 0;
}

static os_err_t _audio_init(device_t *dev)
{
    if (!dev) {
        return OS_FAILURE;
    }

    device_audio_t *dev_audio = (device_audio_t *)dev;

    // 创建停止信号量
    osal_sema_attr_t sema_attr = {"sem_aud_stop", 0, NULL, 0};

    dev_audio->sem_stop = osal_sema_create(1, 0, &sema_attr);
    if (!dev_audio->sem_stop) {
        LOG_ERR("[aud] sem_stop create failed\n");
        return OS_FAILURE;
    }

    // 初始化播放状态
    dev_audio->is_playing = false;

    // 初始化硬件引脚
    if (audio_reset_pin_init(dev_audio) != 0) {
        return OS_FAILURE;
    }
    if (audio_speaker_enable_pin_init(dev_audio) != 0) {
        return OS_FAILURE;
    }

    // 初始化I2C
    if (audio_i2c_init(dev_audio) != 0) {
        return OS_FAILURE;
    }

    // 初始化I2S
    if (audio_i2s_init(dev_audio) != 0) {
        return OS_FAILURE;
    }

#if CONFIG_USING_I2S_BCK_CONTINUOUS_OUTPUT
    device_control(dev_audio->info.i2s_bus, I2S_CMD_START_TRANS_ZERO, NULL);
#endif

    // 调用设备特定初始化
    if (dev_audio->ops && dev_audio->ops->init) {
        dev_audio->ops->init(dev_audio);
    }

    return OS_SUCCESS;
}

static os_ssize_t _audio_write(device_t *dev, os_off_t pos, const void *buffer, os_size_t size)
{
    if (!dev || !buffer) {
        return 0;
    }

    device_audio_t *dev_audio = (device_audio_t *)dev;
    if (dev_audio->ops && dev_audio->ops->transimit) {
        return dev_audio->ops->transimit(dev_audio, (int8_t *)buffer, (uint32_t)size);
    }
    return 0;
}

static os_err_t _audio_control(struct os_device *dev, int cmd, void *args)
{
    if (!dev) {
        return OS_FAILURE;
    }

    os_err_t result = OS_SUCCESS;
    device_audio_t *dev_audio = (device_audio_t *)dev;

    switch (cmd) {

        case AUDIO_CMD_SET_EOF_CB:
            dev_audio->eof_cb = (audio_eof_cb_func)args;
            break;

        case AUDIO_CMD_SPEAKER_POWER:
            if (args) {
                set_speaker_enable_pin(dev_audio, *((bool *)args));
            }
            break;

        default:
            break;
    }

    if (dev_audio->ops && dev_audio->ops->control) {
        result = dev_audio->ops->control(dev_audio, cmd, args);
        if (result != OS_SUCCESS) {
            return OS_FAILURE;
        }
    }
    return result;
}

static const struct device_ops device_audio_ops = {
    .init = _audio_init,
    .write = _audio_write,
    .control = _audio_control,
};

os_err_t device_audio_register(device_audio_t *device, const char *name)
{
    if (!device || !name) {
        return OS_FAILURE;
    }

    os_err_t result = OS_SUCCESS;

    device->parent.ops = &device_audio_ops;
    device->parent.type = DEVICE_TYPE_SOUND;

    result = device_register(&device->parent, name);

    return result;
}