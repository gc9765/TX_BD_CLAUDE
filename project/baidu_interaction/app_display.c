#include "sys_config.h"
#include "typesdef.h"
#include "dev.h"
#include "devid.h"
#include "hal/pwm.h"
#include "hal/audac.h"
#include "osal/string.h"
#include "syscfg.h"
#include "app_display.h"

static struct hgpwm_v0 *s_bl_pwm = NULL;
static uint8_t s_brightness = 5;

/* DAC 数字增益表 0~10 级 */
static const uint32 dacgain_table[] = {
    0, 0x40, 0x80, 0x100, 0x140, 0x180,
    0x200, 0x280, 0x300, 0x480, 0x500
};
static uint8_t s_volume = 0;

/* 音量档位映射: 静音/低/中/高/最高 → DAC gain */
static const uint8_t vol_values[] = { 0, 3, 5, 8, 10 };
#define VOL_IDX_COUNT  5

/* 初始音量: 在 audio_da_init 之后调用, 设置 DAC 增益 */
void volume_init(void)
{
    uint8_t idx = sys_cfgs.user_param1 & 0xFF;
    if (idx >= 1 && idx < VOL_IDX_COUNT) {
        os_printf("[volume] init: saved idx=%d vol=%d\r\n", idx, vol_values[idx]);
    } else {
        /* 默认"中" (idx=2 → vol=5) */
        idx = 2;
        os_printf("[volume] init: default medium\r\n");
    }
    volume_adjust(vol_values[idx]);
}

void lcd_backlight_init(void)
{
    s_bl_pwm = (struct hgpwm_v0 *)dev_get(HG_PWM0_DEVID);
    if (!s_bl_pwm) {
        os_printf("[display] PWM0 dev not found\r\n");
        return;
    }

    pwm_init((struct pwm_device *)s_bl_pwm, PWM_CHANNEL_0, 5, 5);
    pwm_start((struct pwm_device *)s_bl_pwm, PWM_CHANNEL_0);

    /* Restore brightness from flash */
    uint8_t saved = (sys_cfgs.user_param1 >> 8) & 0xFF;
    if (saved >= 1 && saved <= 5) s_brightness = saved;

    lcd_set_brightness(s_brightness);
    os_printf("[display] backlight init, brightness=%d\r\n", s_brightness);
}

void lcd_set_brightness(uint8_t level)
{
    if (level < 1) level = 1;
    if (level > 5) level = 5;
    s_brightness = level;

    s_bl_pwm = (struct hgpwm_v0 *)dev_get(HG_PWM0_DEVID);
    if (!s_bl_pwm) return;

    int8_t duty_level = level + 1;
    uint32_t duty = (uint32_t)duty_level * 1023;

    pwm_ioctl((struct pwm_device *)s_bl_pwm, PWM_CHANNEL_0,
              PWM_IOCTL_CMD_SET_PERIOD_DUTY_IMMEDIATELY, 1023 * 5, duty);
}

uint8_t lcd_get_brightness(void)
{
    return s_brightness;
}

void volume_adjust(uint8_t vol)
{
    if (vol > 10) vol = 10;
    if (s_volume == vol) return;
    s_volume = vol;

    struct audac_device *audac = (struct audac_device *)dev_get(HG_AUDAC_DEVID);
    if (!audac) {
        os_printf("[volume] audac dev not found!\r\n");
        return;
    }
    audac_ioctl(audac, AUDAC_IOCTL_CMD_SET_DIGITAL_GAIN, dacgain_table[vol], 0);
    os_printf("[volume] set gain=%d (reg=0x%x)\r\n", vol, dacgain_table[vol]);
}
