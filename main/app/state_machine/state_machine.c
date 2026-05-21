#include "state_machine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "websocket.h"
#include "app_sr.h"
#include "app_camera.h"

static const char *TAG = "STATE_MACHINE";

static QueueHandle_t event_queue = NULL;
static TaskHandle_t sm_task_handle = NULL;

static state_t current_state = STATE_IDLE;
static int question_index = 1;

// 前向声明
static void state_machine_task(void *pvParams);
static void process_event(state_event_t event);
static void send_anwser_question(int idx);
static void send_end_anwser_question(int idx);
static void start_recording_for_answer(void);
static void stop_recording_and_next_question(void);
static void take_photo_and_send(void);

esp_err_t state_machine_init(void)
{
    event_queue = xQueueCreate(10, sizeof(state_event_t));
    if (!event_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_FAIL;
    }

    xTaskCreate(state_machine_task, "state_machine", 4096, NULL, 5, &sm_task_handle);
    ESP_LOGI(TAG, "State machine initialized, initial state: IDLE");
    return ESP_OK;
}

void state_machine_send_event(state_event_t event)
{
    if (event_queue) {
        xQueueSend(event_queue, &event, 0);
    }
}

int state_machine_get_current_question_index(void)
{
    return question_index;
}

static void state_machine_task(void *pvParams)
{
    state_event_t event;
    while (1) {
        if (xQueueReceive(event_queue, &event, portMAX_DELAY) == pdTRUE) {
            process_event(event);
        }
    }
}

static void process_event(state_event_t event)
{
    ESP_LOGI(TAG, "Event: %d, Current state: %d", event, current_state);

    switch (current_state) {
    case STATE_IDLE:
        if (event == EVENT_RFID_CARD_DETECTED) {
            ws_send_text("{\"type\":\"milestones_anwser_1\"}", strlen("{\"type\":\"milestones_anwser_1\"}"));
            current_state = STATE_MILESTONE_WAIT_START;
            ESP_LOGI(TAG, "State -> MILESTONE_WAIT_START");
        }
        break;

    case STATE_MILESTONE_WAIT_START:
        if (event == EVENT_WS_MILESTONE_STARTED) {
            current_state = STATE_WAIT_KEY_FOR_ANSWER;
            question_index = 1;
            ESP_LOGI(TAG, "State -> WAIT_KEY_FOR_ANSWER (q=%d)", question_index);
        }
        break;

    case STATE_WAIT_KEY_FOR_ANSWER:
        if (event == EVENT_KEY_PRESS_RELEASE) {
            if (!app_sr_is_api_recording()) {
                send_anwser_question(question_index);
                start_recording_for_answer();
                current_state = STATE_RECORDING_ANSWER;
                ESP_LOGI(TAG, "State -> RECORDING_ANSWER (q=%d)", question_index);
            } else {
                ESP_LOGW(TAG, "Ignoring key press because recording is still active (state mismatch)");
            }
        } else if (event == EVENT_WS_ANSWER_PHOTO) {
            // 服务器要求拍照，直接进入拍照等待状态
            if (app_sr_is_api_recording()) {
                app_sr_stop_api_recording();
            }
            current_state = STATE_WAIT_KEY_FOR_PHOTO;
            ESP_LOGI(TAG, "State -> WAIT_KEY_FOR_PHOTO (photo command during wait)");
        }
        break;

    case STATE_RECORDING_ANSWER:
        if (event == EVENT_KEY_PRESS_RELEASE) {
            stop_recording_and_next_question();
        } else if (event == EVENT_WS_ANSWER_PHOTO) {
            // 服务器要求拍照：停止录音，发送结束消息，切换到拍照等待状态
            app_sr_stop_api_recording();
            // 可选：发送当前问题的结束消息（根据协议决定是否需要）
            // send_end_anwser_question(question_index);
            current_state = STATE_WAIT_KEY_FOR_PHOTO;
            ESP_LOGI(TAG, "State -> WAIT_KEY_FOR_PHOTO (audio stream closed)");
        }
        break;

    case STATE_WAIT_KEY_FOR_PHOTO:
        if (event == EVENT_KEY_PRESS_RELEASE) {
            // 防御：确保录音已完全停止
            if (app_sr_is_api_recording()) {
                app_sr_stop_api_recording();
            }
            take_photo_and_send();
            // 拍照后等待 milestone_complete 事件，状态保持不变
        }
        break;

    default:
        break;
    }

    // 全局事件：milestone_complete 在任何状态收到后都应回到 IDLE
    if (event == EVENT_WS_MILESTONE_COMPLETE) {
        if (app_sr_is_api_recording()) {
            app_sr_stop_api_recording();
        }
        current_state = STATE_IDLE;
        question_index = 1;
        ESP_LOGI(TAG, "Milestone complete, back to IDLE");
    }
}

// 辅助函数
static void send_anwser_question(int idx)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"type\":\"anwser_question_%d\"}", idx);
    ws_send_text(buf, strlen(buf));
    ESP_LOGI(TAG, "Sent: %s", buf);
}

static void send_end_anwser_question(int idx)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"type\":\"end_anwser_question_%d\"}", idx);
    ws_send_text(buf, strlen(buf));
    ESP_LOGI(TAG, "Sent: %s", buf);
}

static void start_recording_for_answer(void)
{
    app_sr_start_api_recording(120000);
    ESP_LOGI(TAG, "Recording started for answer (120s)");
}

static void stop_recording_and_next_question(void)
{
    app_sr_stop_api_recording();
    send_end_anwser_question(question_index);
    question_index++;
    current_state = STATE_WAIT_KEY_FOR_ANSWER;
    ESP_LOGI(TAG, "State -> WAIT_KEY_FOR_ANSWER (next question %d)", question_index);
}

static void take_photo_and_send(void)
{
    send_camera_image();
    ESP_LOGI(TAG, "Photo taken and sent, waiting for milestone_complete");
}