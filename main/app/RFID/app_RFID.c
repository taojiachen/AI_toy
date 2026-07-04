#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "pn532_driver_hsu.h"      // 使用 UART 模式
#include "pn532.h"
#include "app_RFID.h"

// ==================== UART 引脚配置（与您提供的 HSU 一致） ====================
#define RESET_PIN      (-1)
#define IRQ_PIN        (-1)
#define HSU_HOST_RX    (20)
#define HSU_HOST_TX    (21)
#define HSU_UART_PORT  UART_NUM_1
#define HSU_BAUD_RATE  (115200)
#define UID_STR_BUFFER_SIZE_MAX (32)

static const char *TAG = "rfid_pn532";

// 引用原 RC522 版本中的全局变量（在 event.c 中定义）
extern struct Nearest_Task {
    char *key;
    char *datavalue;
    int starttime;
    int keeptime;
    int average_time;
} Nearest_Task;
extern int flag;

// PN532 设备句柄
static pn532_io_t g_pn532_io_dev;
static pn532_io_t *g_pn532_io = NULL;
static bool g_rfid_running = false;
static TaskHandle_t rfid_task_handle = NULL;

// ==================== RFID 读卡任务 ====================
static void rfid_task(void *pvParameters)
{
    if (g_pn532_io == NULL) {
        ESP_LOGE(TAG, "rfid_task: PN532 not initialized");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "RFID task started, waiting for card...");

    bool card_present = false;   // 当前是否有卡片在感应区

    while (g_rfid_running) {
        uint8_t uid[7] = {0};
        uint8_t uid_length = 0;

        // 尝试读取 ISO14443A 卡片，超时 500ms
        esp_err_t err = pn532_read_passive_target_id(g_pn532_io,
                                                     PN532_BRTY_ISO14443A_106KBPS,
                                                     uid, &uid_length, 500);
        if (err == ESP_OK) {
            // 检测到卡片
            if (!card_present) {
                // 卡片刚进入
                ESP_LOGI(TAG, "Card detected, UID length: %d", uid_length);
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, uid, uid_length, ESP_LOG_INFO);

                // 将 UID 转换为字符串（去掉空格，与原 RC522 的 removeSpaces 一致）
                char uid_str[UID_STR_BUFFER_SIZE_MAX] = {0}; // 借用原宏定义，若无则用 32
                for (int i = 0; i < uid_length; i++) {
                    char hex[3];
                    sprintf(hex, "%02X", uid[i]);  // 大写的十六进制，与原 RC522 输出一致（如 5B 9D 44 0C）
                    strcat(uid_str, hex);
                    if (i < uid_length - 1) strcat(uid_str, " ");
                }
                // 去除空格（原 removeSpaces 会移除所有空格，但原比较是直接比较去空格后的字符串）
                // 原 RC522 的 removeSpaces 是去掉所有空格，然后比较。我们这里也去掉空格。
                // 但原比较时 datavalue 也是去空格后的，所以保持一致。
                char uid_no_space[32] = {0};
                int j = 0;
                for (int i = 0; uid_str[i] != '\0'; i++) {
                    if (uid_str[i] != ' ') {
                        uid_no_space[j++] = uid_str[i];
                    }
                }
                uid_no_space[j] = '\0';

                ESP_LOGI(TAG, "UID string (no spaces): %s", uid_no_space);
                ESP_LOGI(TAG, "Nearest_Task.datavalue: %s", Nearest_Task.datavalue);

                // 与 Nearest_Task.datavalue 比较
                if (Nearest_Task.datavalue != NULL && 
                    strcmp(Nearest_Task.datavalue, uid_no_space) == 0) {
                    ESP_LOGI(TAG, "Card UID matched!");
                    flag = 1;
                } else {
                    ESP_LOGI(TAG, "Card UID mismatch.");
                    flag = 0;
                }

                card_present = true;
            }
            // 如果卡片仍存在，不做额外操作，等待离开
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            // 读卡超时或失败 → 卡片可能已离开
            if (card_present) {
                ESP_LOGI(TAG, "Card removed.");
                flag = 0;
                card_present = false;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    // 任务退出
    if (g_pn532_io) {
        pn532_release(g_pn532_io);
        g_pn532_io = NULL;
    }
    rfid_task_handle = NULL;
    vTaskDelete(NULL);
}

// ==================== 对外接口 ====================

void RFID_start(void)
{
    esp_err_t err;

    if (rfid_task_handle != NULL) {
        ESP_LOGW(TAG, "RFID already started");
        return;
    }

    // 初始化 PN532 HSU 驱动
    ESP_LOGI(TAG, "Initializing PN532 in HSU (UART) mode");
    err = pn532_new_driver_hsu(HSU_HOST_RX, HSU_HOST_TX, RESET_PIN, IRQ_PIN,
                               HSU_UART_PORT, HSU_BAUD_RATE, &g_pn532_io_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HSU driver init failed: %s", esp_err_to_name(err));
        return;
    }

    // 初始化 PN532 芯片（含重试）
    do {
        err = pn532_init(&g_pn532_io_dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PN532 init failed, retry...");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    } while (err != ESP_OK);

    // 获取固件版本
    uint32_t version_data = 0;
    do {
        err = pn532_get_firmware_version(&g_pn532_io_dev, &version_data);
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "Didn't find PN53x board, resetting...");
            pn532_reset(&g_pn532_io_dev);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    } while (err != ESP_OK);

    ESP_LOGI(TAG, "Found chip PN5%x, Firmware ver. %d.%d",
             (unsigned int)(version_data >> 24) & 0xFF,
             (int)(version_data >> 16) & 0xFF,
             (int)(version_data >> 8) & 0xFF);

    g_pn532_io = &g_pn532_io_dev;
    g_rfid_running = true;

    xTaskCreate(rfid_task, "rfid_task", 3 * 1024, NULL, 5, &rfid_task_handle);
    ESP_LOGI(TAG, "RFID started successfully.");
}

void RFID_stop(void)
{
    g_rfid_running = false;

    if (rfid_task_handle != NULL) {
        // 等待任务自行退出
        int timeout = 100;
        while (rfid_task_handle != NULL && timeout-- > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (rfid_task_handle != NULL) {
            vTaskDelete(rfid_task_handle);
            rfid_task_handle = NULL;
        }
    }

    if (g_pn532_io) {
        pn532_release(g_pn532_io);
        g_pn532_io = NULL;
    }
    ESP_LOGI(TAG, "RFID stopped.");
}