#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <esp_log.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"
#include "pn532_driver_i2c.h"
#include "pn532_driver_hsu.h"
#include "pn532_driver_spi.h"
#include "pn532.h"

// ==================== 接口模式选择（仅选一个为 1） ====================
#define PN532_MODE_I2C   0
#define PN532_MODE_HSU   1
#define PN532_MODE_SPI   0

// ==================== 各模式引脚配置 ====================
#if PN532_MODE_I2C
    #define SCL_PIN      (0)
    #define SDA_PIN      (1)
    #define RESET_PIN    (-1)
    #define IRQ_PIN      (3)

#elif PN532_MODE_HSU
    #define RESET_PIN      (-1)
    #define IRQ_PIN        (-1)
    #define HSU_HOST_RX    (20)
    #define HSU_HOST_TX    (21)
    #define HSU_UART_PORT  UART_NUM_1
    #define HSU_BAUD_RATE  (115200)

#elif PN532_MODE_SPI
    #define RESET_PIN      (-1)
    #define IRQ_PIN        (6)
    #define SPI_CS         (5)
    #define SPI_SCK        (2)
    #define SPI_MISO       (3)
    #define SPI_MOSI       (4)
    #define SPI_HOST_NFC   SPI3_HOST
    #define SPI_CLOCKRATE  1000000
#endif

static const char *TAG = "ntag_read";

// ==================== 全局变量 ====================
static pn532_io_t g_pn532_io_dev;         // 静态全局设备结构体
static pn532_io_t *g_pn532_io = NULL;     // 全局指针
static bool g_rfid_running = false;
static TaskHandle_t rfid_task_handle = NULL;

// ==================== 读卡任务 ====================
void rfid_task(void *pvParameters)
{
    if (g_pn532_io == NULL) {
        ESP_LOGE(TAG, "rfid_task: PN532 not initialized");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Waiting for an ISO14443A Card ...");

    bool card_present = false;   // 当前是否有卡片在感应区

    while (g_rfid_running)
    {
        uint8_t uid[7] = {0};
        uint8_t uid_length = 0;

        // 设置超时为 500ms，快速检测离开
        esp_err_t err = pn532_read_passive_target_id(g_pn532_io,
                                                     PN532_BRTY_ISO14443A_106KBPS,
                                                     uid, &uid_length, 500);
        if (ESP_OK == err)
        {
            if (!card_present)   // 卡片刚进入
            {
                ESP_LOGI(TAG, "\nCard detected (ISO14443A)");
                ESP_LOGI(TAG, "UID Length: %d bytes", uid_length);
                ESP_LOGI(TAG, "UID Value:");
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, uid, uid_length, ESP_LOG_INFO);

                // ---------- 读取 NTAG 数据（仅执行一次） ----------
                err = pn532_in_list_passive_target(g_pn532_io);
                if (err != ESP_OK) {
                    ESP_LOGI(TAG, "Failed to inList passive target");
                    card_present = true;   // 仍标记卡片存在，但本次不重复读
                    continue;
                }

                NTAG2XX_MODEL ntag_model = NTAG2XX_UNKNOWN;
                err = ntag2xx_get_model(g_pn532_io, &ntag_model);
                if (err != ESP_OK) {
                    ESP_LOGI(TAG, "Not an NTAG or failed to read model");
                    card_present = true;
                    continue;
                }

                int page_max = 0;
                switch (ntag_model) {
                    case NTAG2XX_NTAG213:
                        page_max = 45;
                        ESP_LOGI(TAG, "Found NTAG213 (or NTAG203)");
                        break;
                    case NTAG2XX_NTAG215:
                        page_max = 135;
                        ESP_LOGI(TAG, "Found NTAG215");
                        break;
                    case NTAG2XX_NTAG216:
                        page_max = 231;
                        ESP_LOGI(TAG, "Found NTAG216");
                        break;
                    default:
                        ESP_LOGI(TAG, "Unknown NTAG model");
                        card_present = true;
                        continue;
                }

                // 逐 4 页读取（每次读取 16 字节）
                for (int page = 0; page < page_max; page += 4) {
                    uint8_t buf[16] = {0};
                    err = ntag2xx_read_page(g_pn532_io, page, buf, 16);
                    if (err == ESP_OK) {
                        ESP_LOGI(TAG, "Page %d - %d:", page, page + 3);
                        ESP_LOG_BUFFER_HEXDUMP(TAG, buf, 16, ESP_LOG_INFO);
                    } else {
                        ESP_LOGI(TAG, "Failed to read page %d", page);
                        break;   // 发生错误时停止读取
                    }
                }
                // ---------------------------------------------------
                card_present = true;   // 标记卡片存在，等待离开
            }
            // 如果卡片仍存在，不重复执行任何操作
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        else
        {
            // 读卡超时或失败 → 卡片可能已离开
            if (card_present)
            {
                ESP_LOGI(TAG, "Card removed.");
                card_present = false;
            }
            // 短暂延时，避免空转
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    // 任务退出时释放资源
    if (g_pn532_io) {
        pn532_release(g_pn532_io);
        g_pn532_io = NULL;
    }
    rfid_task_handle = NULL;
    vTaskDelete(NULL);
}

// ==================== RFID 初始化 ====================
void RFID_start(void)
{
    esp_err_t err;

    printf("APP RFID Init\n");

#if PN532_MODE_I2C
    ESP_LOGI(TAG, "init PN532 in I2C mode");
    err = pn532_new_driver_i2c(SDA_PIN, SCL_PIN, RESET_PIN, IRQ_PIN, 0, &g_pn532_io_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver init failed: %s", esp_err_to_name(err));
        return;
    }

#elif PN532_MODE_HSU
    ESP_LOGI(TAG, "init PN532 in HSU mode");
    err = pn532_new_driver_hsu(HSU_HOST_RX, HSU_HOST_TX, RESET_PIN, IRQ_PIN,
                               HSU_UART_PORT, HSU_BAUD_RATE, &g_pn532_io_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HSU driver init failed: %s", esp_err_to_name(err));
        return;
    }

#elif PN532_MODE_SPI
    ESP_LOGI(TAG, "init PN532 in SPI mode");
    err = pn532_new_driver_spi(SPI_MISO, SPI_MOSI, SPI_SCK, SPI_CS, -1, IRQ_PIN,
                               SPI_HOST_NFC, SPI_CLOCKRATE, &g_pn532_io_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI driver init failed: %s", esp_err_to_name(err));
        return;
    }
#endif

    // 初始化 PN532 芯片（各模式共用）
    do {
        err = pn532_init(&g_pn532_io_dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to init PN532, retry...");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    } while (err != ESP_OK);

    // 获取固件版本
    uint32_t version_data = 0;
    do {
        err = pn532_get_firmware_version(&g_pn532_io_dev, &version_data);
        if (ESP_OK != err) {
            ESP_LOGI(TAG, "Didn't find PN53x board");
            pn532_reset(&g_pn532_io_dev);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    } while (ESP_OK != err);

    ESP_LOGI(TAG, "Found chip PN5%x", (unsigned int)(version_data >> 24) & 0xFF);
    ESP_LOGI(TAG, "Firmware ver. %d.%d",
             (int)(version_data >> 16) & 0xFF, (int)(version_data >> 8) & 0xFF);

    // 设置全局指针和运行标志
    g_pn532_io = &g_pn532_io_dev;
    g_rfid_running = true;
    ESP_LOGI(TAG, "PN532 initialized successfully.");

    if (rfid_task_handle != NULL) {
        ESP_LOGW(TAG, "RFID scan task already running");
        return;
    }
    xTaskCreate(rfid_task, "rfid_task", 4096, NULL, 5, &rfid_task_handle);
}

// ==================== RFID 停止 ====================
void RFID_stop(void)
{
    g_rfid_running = false;

    if (rfid_task_handle != NULL) {
        int timeout = 100;
        while (rfid_task_handle != NULL && timeout-- > 0) {
            vTaskDelay(10 / portTICK_PERIOD_MS);
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
}
