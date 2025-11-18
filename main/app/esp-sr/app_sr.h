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