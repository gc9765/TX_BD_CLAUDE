#ifndef __PRINTER_KEY_H__
#define __PRINTER_KEY_H__

#include "typesdef.h"

/* 按键动作类型 */
typedef enum {
    KEY_ACT_SHORT,       /* 短按 */
    KEY_ACT_DOUBLE,      /* 双击 */
    KEY_ACT_LONG_START,  /* 长按开始 */
    KEY_ACT_LONG_END,    /* 长按释放 */
} key_action_t;

/* 按键动作回调：上层注册此回调来接收翻译后的按键动作 */
typedef void (*key_action_handler)(key_action_t action);

/**
 * @brief  初始化按键模块，注册回调到底层 keyWork 系统
 * @param  handler  上层按键动作处理函数
 */
void printer_key_init(key_action_handler handler);

/**
 * @brief  反初始化按键模块
 */
void printer_key_deinit(void);

#endif /* __PRINTER_KEY_H__ */
