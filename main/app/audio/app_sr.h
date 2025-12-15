/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_afe_sr_models.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define SR_CONTINUE_DET 1
#define DURATION_PER_FRAME_MS 60     // 每帧时长（ms）
#define BYTES_PER_FRAME  (DURATION_PER_FRAME_MS * CONFIG_OPUS_AUDIO_SAMPLE_RATE / 1000 * sizeof(int16_t))   // 每帧字节数1920字节

#define SAMPLES_PER_BUFFER (WS_TRANSFER_SIZE / sizeof(int16_t)) // 每个缓冲区的样本数

    /**
     * @brief Start speech recognition task
     *
     * @param record_en Record audio to SD crad if set to `true`
     * @return
     *    - ESP_OK: Success
     *    - ESP_ERR_NO_MEM: No enough memory for speech recognition
     *    - Others: Fail
     */
    esp_err_t app_sr_start(void);

#ifdef __cplusplus
}
#endif