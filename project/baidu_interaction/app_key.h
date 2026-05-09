#ifndef __APP_KEY_H__
#define __APP_KEY_H__

#include "typesdef.h"

/* ========== 按键 ID ========== */
typedef enum {
    KEY_ID_OK,      /* SW1 — OK/确认键 */
    KEY_ID_UP,      /* SW2 — 上键 */
    KEY_ID_AI,      /* SW3 — AI键 */
    KEY_ID_DOWN,    /* SW4 — 下键 */
    KEY_ID_M,       /* SW5 — M/菜单键 */
    KEY_ID_POWER,   /* PA1 — 电源键 */
    KEY_ID_COUNT,
} key_id_t;

/* ========== 按键事件 ========== */
typedef enum {
    KEY_EVT_SHORT,       /* 短按释放 */
    KEY_EVT_LONG_START,  /* 长按开始 */
    KEY_EVT_LONG_END,    /* 长按释放 */
} key_event_t;

/* 按键事件回调：上层注册此回调接收翻译后的按键事件 */
typedef void (*key_action_handler)(key_id_t id, key_event_t evt);

/**
 * @brief  初始化按键模块，注册回调到底层 keyWork 系统
 * @param  handler  上层按键事件处理函数
 */
void app_key_init(key_action_handler handler);

/**
 * @brief  反初始化按键模块
 */
void app_key_deinit(void);

#endif /* __APP_KEY_H__ */
