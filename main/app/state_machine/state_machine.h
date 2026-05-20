#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include "esp_err.h"

// 事件类型
typedef enum {
    EVENT_NONE,
    EVENT_RFID_CARD_DETECTED,   // RFID 检测到卡片
    EVENT_KEY_PRESS_RELEASE,    // 按键按下并松开（一次完整操作）
    EVENT_WS_MILESTONE_STARTED, // 服务器返回 {"type":"milestones_anwser_1","status":"started"}
    EVENT_WS_ANSWER_PHOTO,      // 服务器返回 {"type":"anwser_question_photo"}
    EVENT_WS_MILESTONE_COMPLETE,// 服务器返回 {"type":"milestone_complete","milestone":1}
} state_event_t;

// 状态机状态
typedef enum {
    STATE_IDLE,
    STATE_MILESTONE_WAIT_START,
    STATE_WAIT_KEY_FOR_ANSWER,
    STATE_RECORDING_ANSWER,
    STATE_WAIT_KEY_END_ANSWER,
    STATE_WAIT_PHOTO,
    STATE_WAIT_KEY_FOR_PHOTO,
} state_t;

/**
 * @brief 初始化状态机
 */
esp_err_t state_machine_init(void);

/**
 * @brief 发送事件给状态机
 */
void state_machine_send_event(state_event_t event);

/**
 * @brief 设置当前回答问题索引（外部可读）
 */
int state_machine_get_current_question_index(void);

#endif