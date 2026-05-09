#ifndef __PRINTER_VOICE_H__
#define __PRINTER_VOICE_H__

#include <stdint.h>

/* 6 种打印模式 */
typedef enum {
    MODE_SKETCH_PEN = 0,   // 简笔画
    MODE_LINE_ART   = 1,   // 线稿
    MODE_PENCIL     = 2,   // 素描
    MODE_ANIME      = 3,   // 动漫
    MODE_CARTOON    = 4,   // 卡通
    MODE_INK_WASH   = 5,   // 水墨
    MODE_COUNT      = 6
} printer_mode_t;

/**
 * @brief  初始化语音播报模块
 */
void printer_voice_init(void);

/**
 * @brief  播报指定模式名称（异步，后台线程播放）
 * @param  mode  要播报的模式
 */
void printer_voice_announce(printer_mode_t mode);

/**
 * @brief  等待当前播报完成
 */
void printer_voice_wait_done(void);

/**
 * @brief  反初始化语音播报模块
 */
void printer_voice_deinit(void);

/**
 * @brief  获取当前打印模式
 */
printer_mode_t printer_get_mode(void);

#endif /* __PRINTER_VOICE_H__ */
