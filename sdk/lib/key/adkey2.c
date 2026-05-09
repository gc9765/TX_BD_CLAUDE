#include "sys_config.h"
#include "typesdef.h"
#include "adkey.h"
#include "keyScan.h"

#include "hal/gpio.h"
#include "osal/string.h"


/*********************************************************
 *  拍学机 PA1 电源键 — GPIO 模式
 *
 *  参考: intercom_test_v3/sdk/lib/key/iokey.c
 *
 *  PA1 为电源开关键，按下接 VCC（高电平），松开通过外部下拉为低。
 *  使用 GPIO 读取电平，不经过 ADC。
 *
 *  按键行为:
 *    - 长按 → 关机 (软件检测长按后调用 power_off)
 *    - 短按 → UI 功能键
 *
 *  PA1 按键电路: SW7 → R53(33K) → PA1, R56(100K) pulldown to GND
 *  按下时: GPIO = 1 (高电平)
 *  松开时: GPIO = 0 (低电平)
 ************************************************************/

/* 与参考工程 iokey.h 保持一致 */
struct iokey_t {
    void   *priv;
    uint32  pin;
    uint8   pull;
    uint8   pull_level;
    uint8   invert;        /* 1=高电平表示按下, 0=低电平表示按下 */
    uint8   keycode;
};

/* 与参考工程 iokey.c init 完全一致: 只调 gpio_set_mode */
static void key_iokey_init(key_channel_t *key, uint8_t enable)
{
    struct iokey_t *iokey = (struct iokey_t *)key->priv;

    if (enable) {
        gpio_set_mode(iokey->pin, iokey->pull, iokey->pull_level);
        os_printf("[iokey] init pin=%d pull=%d\n", iokey->pin, iokey->pull);
        key->enable = 1;
    } else {
        gpio_set_mode(iokey->pin, GPIO_PULL_NONE, 0);
        os_printf("[iokey] deinit pin=%d\n", iokey->pin);
        key->enable = 0;
    }
}

/* 与参考工程 iokey.c scan 完全一致 */
static uint8 key_iokey_scan(key_channel_t *key)
{
    struct iokey_t *iokey = (struct iokey_t *)key->priv;
    uint32 gpio_val = gpio_get_val(iokey->pin);
    key->extern_value = gpio_val;

    if (iokey->invert) {
        /* 反相: 高电平表示按下 */
        return gpio_val ? iokey->keycode : KEY_NONE;
    } else {
        /* 正常: 低电平表示按下 */
        return gpio_val ? KEY_NONE : iokey->keycode;
    }
}


static const keys_t iokey_arg = {
    .period_long     = 1000,  /* 长按判定 1s (关机触发) */
    .period_repeat   = 1000,
    .period_dither   = 80,
};

/* PA1 电源键: 外部已有 R56(100K) 下拉，无需芯片内部下拉 */
static struct iokey_t iokey_power = {
    .priv       = NULL,
    .pin        = PA_1,
    .pull       = GPIO_PULL_NONE,
    .pull_level = GPIO_PULL_LEVEL_NONE,
    .invert     = 1,          /* 高电平 = 按下 */
    .keycode    = AD_C,       /* 映射到 AD_C，在 app_key.c 中转为 KEY_ID_POWER */
};

/* 外部调用 — 名称保持 adkey_key2 与 keyWork.c 中 button_channels[] 一致 */
key_channel_t adkey_key2 = {
    .init       = key_iokey_init,
    .scan       = key_iokey_scan,
    .prepare    = NULL,
    .priv       = (void *)&iokey_power,
    .key_arg    = &iokey_arg,
    .key_table  = NULL,       /* GPIO 按键不需要 lookup table */
};
