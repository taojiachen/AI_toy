#include "driver/i2s_std.h"
#include <bsp_board.h>
#include "driver/i2s_tdm.h"
#include "soc/soc_caps.h"
#include "string.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <esp_spiffs.h>
#include <bsp_err_check.h>

//#define ADC_I2S_CHANNEL 1
static const char *TAG = "bspboard";

i2s_chan_handle_t rx_handle;
i2s_chan_handle_t tx_handle;

void init_i2s_read(void)
{
    // 配置I2S输入通道
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    rx_chan_cfg.dma_desc_num = DMA_BUF_COUNT;
    rx_chan_cfg.dma_frame_num = DMA_BUF_LEN;
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_handle));

    // 配置I2S输入参数
    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RX_RATE),// 1. 采样率改为16000Hz
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,// 2. 采样点数据位宽改为16bit（对应int16）
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,// 3. 槽位宽改为16bit（与数据位宽匹配）
            .slot_mode = I2S_SLOT_MODE_MONO,// 单声道（保持不变）
            .slot_mask = I2S_STD_SLOT_LEFT, // 单声道使用左声道槽位（保持不变）
            .ws_width = 16,// 4. 字时钟宽度改为16（与位宽匹配）
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,         // 5. 小端序（false表示小端）
            .bit_order_lsb = false},
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,// INMP441通常不需要MCLK
            .bclk = INMP441_SCK_PIN,
            .ws = INMP441_WS_PIN,
            .dout = I2S_GPIO_UNUSED,// 输入模式下不使用输出引脚
            .din = INMP441_SD_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &rx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
}

i2s_chan_handle_t init_i2s_write(void)
{
    // 配置I2S输出通道
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = DMA_BUF_COUNT;
    tx_chan_cfg.dma_frame_num = DMA_BUF_LEN;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle, NULL));

    // 配置I2S输出参数
    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_TX_RATE),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = 16,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false},
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MAX98357A_BCLK_PIN,
            .ws = MAX98357A_LRC_PIN,
            .dout = MAX98357A_DIN_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &tx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    return tx_handle;

}

size_t bytes_read = 0;
size_t bytes_written = 0;

// 获取i2s设备(麦克风)数据，并将获取到的数据存入到一个数组中，(i2s通道，数组名，数组长度)
esp_err_t bsp_i2s_read(int16_t *buffer, int buffer_len)
{
    ESP_ERROR_CHECK(i2s_channel_read(rx_handle, buffer,
                                     buffer_len,
                                     &bytes_read, portMAX_DELAY));
    return ESP_OK;
}


// 输出i2s设备(扬声器)数据，并将获取到的数据存入到一个数组中，(i2s通道，数组名，数组长度)
esp_err_t bsp_i2s_write(int16_t *buffer, int buffer_len)
{
    ESP_ERROR_CHECK(i2s_channel_write(tx_handle, buffer,
                                     buffer_len,
                                     &bytes_written, portMAX_DELAY));
    return ESP_OK;
}



esp_err_t bsp_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        // 挂载点路径、分区标签、最大文件数
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 20,
#ifdef CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
    };

    esp_err_t ret_val = esp_vfs_spiffs_register(&conf);

    BSP_ERROR_CHECK_RETURN_ERR(ret_val);

    size_t total = 0, used = 0;
    ret_val = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret_val != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret_val));
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    return ret_val;
}

esp_err_t bsp_spiffs_unmount(void)
{
    return esp_vfs_spiffs_unregister("storage");
}



esp_err_t bsp_board_init()
{
    init_i2s_read();
    init_i2s_write();
    bsp_spiffs_mount();
    return ESP_OK;
}
