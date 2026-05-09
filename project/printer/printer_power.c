#include "sys_config.h"
#include "printer_power.h"

/* PA_0: PWR_ON/OFF — 电源保持引脚
 *
 * 电路原理：
 *   长按按键 → BY25064A1TG 检测长按 → 使能 TM1081 → 3.3V 上电 → MCU 启动
 *   MCU 启动后立即拉高 PA_0 → 保持电源 (HOLD)
 *   关机时拉低 PA_0 → BY25064A1TG 释放 → TM1081 EN 拉低 → 系统断电
 */
#define PWR_HOLD_PIN    PA_0

static uint8_t g_power_on = 1;

/* ========== 公开 API ========== */

void printer_power_early_hold(void)
{
    gpio_set_val(PWR_HOLD_PIN, 1);
    gpio_iomap_output(PWR_HOLD_PIN, GPIO_IOMAP_OUTPUT);
	
	//状态灯
	gpio_set_val(PA_12, 1); 
    gpio_iomap_output(PA_12, GPIO_IOMAP_OUTPUT);
	
}

void printer_power_init(void)
{
    /* 确认电源保持状态（PA_0 已在 early_hold 中初始化） */
    gpio_set_val(PWR_HOLD_PIN, 1);
    g_power_on = 1;
}

void printer_power_off(void)
{
    g_power_on = 0;

	gpio_set_val(PA_12, 0);//断电 关闭指示灯

    /* 关闭外设后拉低 PA_0 → 系统断电 */
    gpio_set_val(PWR_HOLD_PIN, 0);

    while (1) {
        /* 等待断电，不应到达此处 */
    }
}

uint8_t printer_power_is_on(void)
{
    return g_power_on;
}
