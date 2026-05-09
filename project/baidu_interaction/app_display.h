#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include <stdint.h>

/* 初始化 PWM 背光 , 调用前确保 HG_PWM0_DEVID 已注册 */
void lcd_backlight_init(void);

/* 设置背光亮度 1~5 档 */
void lcd_set_brightness(uint8_t level);

/* 获取当前亮度档位 */
uint8_t lcd_get_brightness(void);

/* 设置音量 0~10 级 (DAC 数字增益) */
void volume_adjust(uint8_t vol);

/* 初始化音量 (audio_da_init 之后调用) */
void volume_init(void);

#endif
