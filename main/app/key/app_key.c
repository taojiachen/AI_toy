#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "app_key.h"
#include "state_machine.h"

#define BUTTON_GPIO         19              // 按键引脚
#define DEBOUNCE_DELAY_MS   20              // 消抖延时（毫秒）
#define ESP_INTR_FLAG_DEFAULT 0

static const char *TAG = "button_isr";
static TaskHandle_t button_task_handle = NULL;

// 中断服务函数（必须在 IRAM 中）
static void IRAM_ATTR button_isr_handler(void *arg)
{
    // 仅向消抖任务发送通知，不执行耗时操作
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(button_task_handle, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// 按键消抖与处理任务
static void button_debounce_task(void *pvParameter)
{
    uint32_t ulNotificationValue;
    ESP_LOGI(TAG, "Button ISR task started, monitoring GPIO %d", BUTTON_GPIO);

    while (1) {
        // 等待中断发出的通知（阻塞）
        ulNotificationValue = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (ulNotificationValue > 0) {
            // 消抖：延时后再次读取电平
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_DELAY_MS));
            if (gpio_get_level(BUTTON_GPIO) == 0) {
                // 确认按键有效按下
                ESP_LOGI(TAG, "Button pressed! (ISR + debounce)");
                // ---- 在这里添加你的按键处理代码 ----
                // 例如：控制LED、播放声音等

                // 等待按键释放（防止一次按下多次触发）
                while (gpio_get_level(BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                state_machine_send_event(EVENT_KEY_PRESS_RELEASE);
                ESP_LOGI(TAG, "Button released.");
            }
        }
    }
}

// 在 app_main() 中调用此初始化函数
void button_init(void)
{
    // 1. 配置 GPIO
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,     // 下降沿中断
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .pull_up_en = GPIO_PULLUP_ENABLE,   // 内部上拉
        .pull_down_en = GPIO_PULLDOWN_DISABLE
    };
    gpio_config(&io_conf);

    // 2. 创建消抖任务（必须在安装中断前获得任务句柄）
    xTaskCreate(button_debounce_task, "btn_debounce", 1024, NULL, 10, &button_task_handle);

    // 3. 安装 GPIO 中断服务
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL);

    ESP_LOGI(TAG, "Button GPIO interrupt initialized");
}