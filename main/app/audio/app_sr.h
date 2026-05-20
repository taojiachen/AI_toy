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
#define DURATION_PER_FRAME_MS 60                                                                                 // 每帧时长（ms）
#define BYTES_PER_FRAME (DURATION_PER_FRAME_MS * CONFIG_OPUS_AUDIO_ENCODER_SAMPLE_RATE / 1000 * sizeof(int16_t)) // 每帧字节数1920字节

#define SAMPLES_PER_BUFFER (WS_TRANSFER_SIZE / sizeof(int16_t)) // 每个缓冲区的样本数

    /**
     * @brief Start speech recognition task
     *
     * @return
     *    - ESP_OK: Success
     *    - ESP_ERR_NO_MEM: No enough memory for speech recognition
     *    - Others: Fail
     */
    esp_err_t app_sr_start(void);

    /**
     * @brief 通过 API 启动录音（由状态机调用），可自定义最长录音时长
     * @param duration_ms 录音最大时长（毫秒），例如 120000
     */
    void app_sr_start_api_recording(int duration_ms);

    /**
     * @brief 停止 API 录音（由状态机调用）
     */
    void app_sr_stop_api_recording(void);

    /**
     * @brief 检查是否有 API 录音正在活动
     * @return true 正在录音，false 空闲
     */
    bool app_sr_is_api_recording(void);

#ifdef __cplusplus
}
#endif