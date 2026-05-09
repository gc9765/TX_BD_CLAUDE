#ifndef __PRINTER_POWER_H__
#define __PRINTER_POWER_H__

#include "typesdef.h"

/**
 * @brief  早期电源保持 — 必须在 main() 最开头调用
 *         配置 PA_0 (PWR_ON/OFF) 为输出并拉高，防止 BY25064A1TG 超时断电
 * @note   此函数越早调用越好，迟于 ~1s 可能导致硬件释放电源
 */
void printer_power_early_hold(void);

/**
 * @brief  电源管理模块完整初始化
 *         在 hardware_init() 中调用，确认 PA_0 状态
 */
void printer_power_init(void);

/**
 * @brief  关机：拉低 PA_0 释放电源保持，系统断电
 * @note   调用后系统会立即断电，此函数不会返回
 */
void printer_power_off(void);

/**
 * @brief  检查系统是否处于开机状态
 * @return 1=开机, 0=关机
 */
uint8_t printer_power_is_on(void);

#endif /* __PRINTER_POWER_H__ */
