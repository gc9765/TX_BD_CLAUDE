#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "osal/time.h"
#include "keyWork.h"
#include "keyScan.h"
#include "app_key.h"

/* ========== 配置 ========== */
#define STARTUP_GRACE_MS   2000    /* 启动静默期：忽略开机后前 N ms 的按键事件 */

/* ========== 模块状态 ========== */
static key_action_handler g_handler = NULL;
static struct key_callback_list_s *g_key_cb_handle = NULL;
static volatile uint32_t g_init_tick = 0;

/* ========== SDK 键码 → key_id_t 映射 ========== */

/*
 * PA3 ADKey (adkey_key):
 *   AD_PRESS → KEY_OK    AD_UP → KEY_UP    AD_A → KEY_AI
 *   AD_DOWN  → KEY_DOWN  AD_B → KEY_M
 *
 * PA1 电源键 (adkey_key2):
 *   AD_PRESS → KEY_POWER
 *
 * 两个通道共用同一个 AD_PRESS 键码，通过 extern_value (ADC 原始值)
 * 区分来源：PA1 按下 ADC≈0，PA3 按下 ADC≈0，但 PA1 松开 ADC≈4096，
 * PA3 松开 ADC≈4096。因为分属不同 key_channel_t，SDK 会在不同通道中
 * 扫描，key_code 不会混淆——adkey_key2 只有 AD_PRESS 一个有效键码。
 *
 * 但为安全起见，通过 key_channel 上下文区分：当 adkey_key2 产生 AD_PRESS
 * 时该事件只来自 PA1 通道。
 *
 * 实际上 SDK keyScan 在 key_button_scan 中对每个 channel 独立扫描，
 * keyvalue = (key_code << 8) | key_event，不同通道的 AD_PRESS 不会冲突。
 * 但 SDK 没有传递 channel 信息到回调中。
 *
 * 解决方案：利用 extern_value (ADC 值) 区分 — 两个通道都读 ADC，
 * PA3 的 OK 键按下 ADC≈0，PA1 的电源键按下 ADC≈0，值域重叠。
 * 因此改用 key_code 组合判断：
 *   - adkey_key  通道：AD_PRESS/AD_UP/AD_DOWN/AD_A/AD_B
 *   - adkey_key2 通道：只有 AD_PRESS
 * 无法在回调中区分通道 → 改为：让 adkey_key2 使用不同键码。
 *
 * 修改 adkey2.c 中 adkey_table，将 AD_PRESS 改为 AD_C（空闲键码），
 * 这样 PA1 电源键产生 AD_C，PA3 OK 键产生 AD_PRESS，回调中可区分。
 *
 * 当前代码已基于此假设编写。如 adkey2.c 未修改，PA1 会产生 AD_PRESS，
 * 与 PA3 OK 键冲突，需修改 adkey2.c 或在回调中用其他方式区分。
 */

static key_id_t map_key_id(uint32_t key_code)
{
    switch (key_code) {
    case AD_PRESS:  return KEY_ID_OK;     /* PA3 SW1 OK键 */
    case AD_UP:     return KEY_ID_UP;     /* PA3 SW2 上键 */
    case AD_A:      return KEY_ID_AI;     /* PA3 SW3 AI键 */
    case AD_DOWN:   return KEY_ID_DOWN;   /* PA3 SW4 下键 */
    case AD_B:      return KEY_ID_M;      /* PA3 SW5 M键 */
    case AD_C:      return KEY_ID_POWER;  /* PA1 电源键 */
    default:        return KEY_ID_COUNT;  /* 未知键，忽略 */
    }
}

/* ========== 底层按键回调 ========== */

static uint32_t app_key_callback(struct key_callback_list_s *list,
                                  uint32_t keyvalue, uint32_t extern_value)
{
    uint32_t key_code  = keyvalue >> 8;
    uint32_t key_event = keyvalue & 0xff;

    key_id_t id = map_key_id(key_code);
    if (id >= KEY_ID_COUNT) {
        return 0;   /* 忽略未知键码 */
    }

    /* 启动静默期：忽略开机后前 STARTUP_GRACE_MS 内的所有按键事件 */
    if (g_init_tick && (os_jiffies() - g_init_tick < STARTUP_GRACE_MS)) {
        os_printf("[app_key] grace period, ignore key=%u evt=%u\r\n", key_code, key_event);
        return 0;
    }

    key_event_t evt;

    switch (key_event) {

    case KEY_EVENT_SUP:
        /* 短按释放 */
        evt = KEY_EVT_SHORT;
        os_printf("[app_key] SHORT  id=%u\r\n", id);
        break;

    case KEY_EVENT_LDOWN:
        /* 长按开始 */
        evt = KEY_EVT_LONG_START;
        os_printf("[app_key] LONG_START  id=%u\r\n", id);
        break;

    case KEY_EVENT_LUP:
        /* 长按释放 */
        evt = KEY_EVT_LONG_END;
        os_printf("[app_key] LONG_END  id=%u\r\n", id);
        break;

    case KEY_EVENT_DOWN:
        /* 按下事件 — 不转发，等 SUP/LDOWN 再处理 */
        return 0;

    case KEY_EVENT_REPEAT:
        /* 连按 — 当前不处理 */
        return 0;

    default:
        return 0;
    }

    if (g_handler) {
        g_handler(id, evt);
    }

    return 0;
}

/* ========== 公开 API ========== */

void app_key_init(key_action_handler handler)
{
    g_handler = handler;
    g_init_tick = os_jiffies();

    if (!g_key_cb_handle) {
        g_key_cb_handle = add_keycallback(app_key_callback, NULL);
        os_printf("[app_key] init done, grace=%ums\r\n", STARTUP_GRACE_MS);
    }
}

void app_key_deinit(void)
{
    if (g_key_cb_handle) {
        remove_keycallback(g_key_cb_handle);
        g_key_cb_handle = NULL;
    }
    g_handler = NULL;
    os_printf("[app_key] deinit\r\n");
}
