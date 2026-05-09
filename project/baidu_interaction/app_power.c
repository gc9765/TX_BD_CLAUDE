#include "sys_config.h"
#include "typesdef.h"
#include "dev.h"
#include "devid.h"
#include "osal/string.h"
#include "osal/mutex.h"
#include "osal/semaphore.h"
#include "hal/gpio.h"
#include "hal/adc.h"
#include "dev/adc/hgadc_v0.h"
#include "app_power.h"
#include "osal/sleep.h"

/* ========== 电源保持 (PA0) ========== */

static uint8_t g_power_on = 1;

void app_power_early_hold(void)
{
    /* PA0 拉高保持电源，必须在 main() 最开头调用 */
    gpio_set_val(PWR_HOLD_PIN, 1);
    gpio_iomap_output(PWR_HOLD_PIN, GPIO_IOMAP_OUTPUT);

    /* PC4 功放使能脚拉高，关闭功放防止开机 pop 噪声
     * PC4=0 开启功放, PC4=1 关闭功放 (8002D active-low enable)
     * 播放时由 printer_voice / txw81x_voice 自行管理 PA */
    gpio_set_mode(PA_EN_PIN, GPIO_PULL_NONE, GPIO_PULL_LEVEL_NONE);
    gpio_set_dir(PA_EN_PIN, GPIO_DIR_OUTPUT);
    gpio_set_val(PA_EN_PIN, 1);
}

void app_power_init(void)
{
    /* 确认电源保持状态 */
    gpio_set_val(PWR_HOLD_PIN, 1);
    g_power_on = 1;

    /* 初始化电池检测 ADC (PA2) */
    struct hgadc_v0 *adc = (struct hgadc_v0 *)dev_get(HG_ADC0_DEVID);
    if (adc) {
        adc_open((struct adc_device *)adc);
        gpio_set_mode(BAT_ADC_PIN, GPIO_PULL_NONE, GPIO_PULL_LEVEL_100K);
        adc_add_channel((struct adc_device *)adc, BAT_ADC_PIN);
    }

    os_printf("[power] init done, bat=%umV\r\n", app_battery_voltage_mv());
}

void app_power_off(void)
{
    os_printf("[power] shutting down...\r\n");
    g_power_on = 0;

    /* TODO: 保存 syscfg */

    /* 拉低 PA0 → 硬件释放电源 → 断电 */
    gpio_set_val(PWR_HOLD_PIN, 0);

    while (1) {
        /* 等待断电，不应到达此处 */
    }
}

/* ========== 电池电压检测 (PA2) ========== */

/*
 * 分压电路:
 *   VBAT --- R51(1M) ---+--- R52(1M) --- GND
 *                       |
 *                      PA2 (ADC)
 * R51 = R52 → V_PA2 = V_BAT / 2
 * ADC = 12bit, VREF = 2.7V (TXW81x 内部参考)
 * V_BAT (mV) = 2 × ADC × 2700 / 4096
 */

static uint16_t battery_adc_read(void)
{
    struct hgadc_v0 *adc = (struct hgadc_v0 *)dev_get(HG_ADC0_DEVID);
    uint32 vol = 0;
    if (adc) {
        adc_get_value((struct adc_device *)adc, BAT_ADC_PIN, &vol);
    }
    os_printf("[power] ADC raw=%u (pin=%u)\r\n", vol, BAT_ADC_PIN);
    return (uint16_t)vol;
}

uint16_t app_battery_voltage_mv(void)
{
    uint16_t adc = battery_adc_read();
    /* V_bat = 2 × ADC × 2700 / 4096 */
    uint32_t mv = ((uint32_t)adc * 2 * 2700) / 4096;
    return (uint16_t)mv;
}

enum bat_level app_battery_get_level(void)
{
    uint16_t adc = battery_adc_read();

    if (adc >= BAT_ADC_FULL)     return BAT_LEVEL_FULL;
    if (adc >= BAT_ADC_NORMAL)   return BAT_LEVEL_HIGH;
    if (adc >= BAT_ADC_LOW)      return BAT_LEVEL_MEDIUM;
    if (adc >= BAT_ADC_CRITICAL) return BAT_LEVEL_LOW;
    return BAT_LEVEL_CRITICAL;
}

void app_battery_check(void)
{
    uint16_t mv = app_battery_voltage_mv();

    if (mv < 3000) {
        os_printf("[power] battery critical: %umV, shutting down\r\n", mv);
        os_sleep_ms(100);
        app_power_off();
    } else if (mv < 3400) {
        os_printf("[power] battery low: %umV\r\n", mv);
        /* TODO: 通过 UI 通知用户电量低 */
    }
}
