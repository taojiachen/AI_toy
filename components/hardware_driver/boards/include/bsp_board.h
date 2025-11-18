#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"

// INMP441 引脚定义
#define INMP441_WS_PIN 4
#define INMP441_SCK_PIN 5
#define INMP441_SD_PIN 6

// MAX98357A 引脚定义
#define MAX98357A_LRC_PIN 16
#define MAX98357A_BCLK_PIN 15
#define MAX98357A_DIN_PIN 7

#define SAMPLE_RX_RATE 16000
#define SAMPLE_TX_RATE 24000
#define DMA_BUF_COUNT 8
#define DMA_BUF_LEN 1023

esp_err_t bsp_board_init();

esp_err_t bsp_i2s_read(int16_t *buffer, int buffer_len);
esp_err_t bsp_i2s_write(int16_t *buffer, int buffer_len);

esp_err_t bsp_spiffs_mount(void);

esp_err_t bsp_spiffs_unmount(void);

