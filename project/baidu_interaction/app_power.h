#ifndef __APP_POWER_H__
#define __APP_POWER_H__

#include "typesdef.h"

/* 电源保持引脚: PA0
 * PA1 电源键长按 → 硬件使能电源 → MCU启动 → PA0拉高保持供电
 * 关机时 PA0 拉低 → 硬件释放电源 → 系统断电
 */
#define PWR_HOLD_PIN        PA_0

/* 功放使能引脚: PC4 (8002D PA_EN)
 * 高电平=功放输出使能, 低电平=功放静音
 * 开机时必须先拉低防止啸叫，播放音频时再拉高
 */
#define PA_EN_PIN           PC_4

/* 电池电压检测引脚: PA2
 * 分压电路 1:1 (R51=R52=1M), PA2电压 = 电池电压 / 2
 * ADC = 12bit (0~4095), VREF = 2.7V (TXW81x 内部参考电压)
 * V_battery = 2 × (ADC / 4096) × 2.7
 *
 * 典型电压参考:
 *   4.2V 满电 → PA2=2.1V → ADC≈3186
 *   3.7V 正常 → PA2=1.85V → ADC≈2807
 *   3.4V 偏低 → PA2=1.7V → ADC≈2580
 *   3.0V 欠压 → PA2=1.5V → ADC≈2276
 */
#define BAT_ADC_PIN         PA_2
#define BAT_ADC_FULL        3186    /* ≈4.2V */
#define BAT_ADC_NORMAL      2807    /* ≈3.7V */
#define BAT_ADC_LOW         2580    /* ≈3.4V 低电量警告 */
#define BAT_ADC_CRITICAL    2276    /* ≈3.0V 欠压保护关机 */

/* 电量等级 (用于UI显示) */
enum bat_level {
    BAT_LEVEL_FULL = 0,     /* ≥4.2V */
    BAT_LEVEL_HIGH,         /* ≥3.7V */
    BAT_LEVEL_MEDIUM,       /* ≥3.4V */
    BAT_LEVEL_LOW,          /* ≥3.0V */
    BAT_LEVEL_CRITICAL,     /* <3.0V */
};

/**
 * @brief  早期电源保持 — 必须在 main() 最开头调用
 *         配置 PA0 为输出并拉高，防止电源管理芯片超时断电
 * @note   此函数越早调用越好，迟于 ~1s 可能导致硬件释放电源
 */
void app_power_early_hold(void);

/**
 * @brief  电源管理模块完整初始化
 *         确认 PA0 状态并初始化电池 ADC
 */
void app_power_init(void);

/**
 * @brief  关机：保存配置 → 拉低 PA0 释放电源保持
 * @note   调用后系统会立即断电，此函数不会返回
 */
void app_power_off(void);

/**
 * @brief  读取电池电压 (mV)
 * @return 电池电压，如 3700 表示 3.7V
 */
uint16_t app_battery_voltage_mv(void);

/**
 * @brief  获取电池电量等级
 */
enum bat_level app_battery_get_level(void);

/**
 * @brief  电池检测：低电量自动关机保护
 */
void app_battery_check(void);

#endif /* __APP_POWER_H__ */
