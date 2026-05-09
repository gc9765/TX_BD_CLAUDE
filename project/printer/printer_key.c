#include "sys_config.h"
#include "typesdef.h"
#include "osal/timer.h"
#include "osal/string.h"
#include "osal/time.h"
#include "keyWork.h"
#include "keyScan.h"
#include "printer_key.h"

/* ========== 配置 ========== */
#define DOUBLE_CLICK_MS    300   /* 双击判定窗口 (ms) */
#define TARGET_KEY         AD_A     /* 电源按键 (PA1_PWR_KEY_DET, adkey_key2) */
#define STARTUP_GRACE_MS   2000    /* 启动静默期：忽略开机后前 N ms 的按键事件 */

/* ========== 模块状态 ========== */
static key_action_handler g_action_handler = NULL;
static struct key_callback_list_s *g_key_cb_handle = NULL;

/* 双击检测状态 */
static struct os_timer g_dblclick_timer;
static uint8_t g_dblclick_timer_inited = 0;
static volatile uint8_t g_sup_pending = 0;    /* 有一个 SHORT 等待判定 */
static volatile uint8_t g_long_active = 0;    /* 长按正在进行中 */
static volatile uint32_t g_init_tick = 0;     /* 初始化时的 tick，用于启动静默期 */

/* ========== 内部函数 ========== */

static void notify_action(key_action_t act)
{
    if (g_action_handler) {
        g_action_handler(act);
    }
}

/* 双击超时回调：在等待窗口内没有收到第二次短按 → 判定为短按 */
static void dblclick_timeout(void *arg)
{
    os_printf("[key_cb] dblclick timeout, sup_pending=%u\r\n", g_sup_pending);
    if (g_sup_pending) {
        g_sup_pending = 0;
        notify_action(KEY_ACT_SHORT);
    }
}

/* 底层按键回调 */
static uint32_t printer_key_callback(struct key_callback_list_s *list,
                                     uint32_t keyvalue, uint32_t extern_value)
{
    uint32_t key_code  = keyvalue >> 8;
    uint32_t key_event = keyvalue & 0xff;

    os_printf("[key_cb] raw=0x%04X code=%u event=%u\r\n", keyvalue, key_code, key_event);

    /* 启动静默期：忽略开机后前 STARTUP_GRACE_MS 内的所有按键事件 */
    if (g_init_tick && (os_jiffies() - g_init_tick < STARTUP_GRACE_MS)) {
        os_printf("[key_cb] grace period, ignore (elapsed=%ums)\r\n",
                  (uint32_t)(os_jiffies() - g_init_tick));
        return 0;
    }

    /* 只处理目标按键 */
    if (key_code != TARGET_KEY) {
        return 0;
    }

    switch (key_event) {

    case KEY_EVENT_SUP:
        /* 短按释放 */
        os_printf("[key_cb] SUP, long_active=%u, sup_pending=%u\r\n", g_long_active, g_sup_pending);
        if (g_long_active) {
            /* 长按后的 LUP 也会产生 SUP，忽略 */
            break;
        }
        if (g_sup_pending) {
            /* 窗口内第二次短按 → 双击 */
            os_timer_stop(&g_dblclick_timer);
            g_sup_pending = 0;
            notify_action(KEY_ACT_DOUBLE);
        } else {
            /* 第一次短按，启动双击等待窗口 */
            g_sup_pending = 1;
            os_timer_start(&g_dblclick_timer, DOUBLE_CLICK_MS);
        }
        break;

    case KEY_EVENT_LDOWN:
        /* 长按开始，取消可能 pending 的短按 */
        os_printf("[key_cb] LDOWN\r\n");
        g_long_active = 1;
        if (g_sup_pending) {
            os_timer_stop(&g_dblclick_timer);
            g_sup_pending = 0;
        }
        notify_action(KEY_ACT_LONG_START);
        break;

    case KEY_EVENT_LUP:
        /* 长按释放 */
        os_printf("[key_cb] LUP, long_active=%u\r\n", g_long_active);
        if (g_long_active) {
            g_long_active = 0;
            notify_action(KEY_ACT_LONG_END);
        }
        break;

    default:
        os_printf("[key_cb] unknown event=%u\r\n", key_event);
        break;
    }

    return 0;
}

/* ========== 公开 API ========== */

void printer_key_init(key_action_handler handler)
{
    g_action_handler = handler;
    g_sup_pending = 0;
    g_long_active = 0;
    g_init_tick = os_jiffies();  /* 记录初始化时刻，用于启动静默期 */

    /* 初始化双击检测定时器 (单次触发) */
    if (!g_dblclick_timer_inited) {
        os_timer_init(&g_dblclick_timer, dblclick_timeout, OS_TIMER_MODE_ONCE, NULL);
        g_dblclick_timer_inited = 1;
    }

    /* 注册按键回调到底层 keyWork */
    if (!g_key_cb_handle) {
        g_key_cb_handle = add_keycallback(printer_key_callback, NULL);
    }
}

void printer_key_deinit(void)
{
    if (g_key_cb_handle) {
        remove_keycallback(g_key_cb_handle);
        g_key_cb_handle = NULL;
    }
    if (g_dblclick_timer_inited) {
        os_timer_stop(&g_dblclick_timer);
        os_timer_del(&g_dblclick_timer);
        g_dblclick_timer_inited = 0;
    }
    g_action_handler = NULL;
    g_sup_pending = 0;
    g_long_active = 0;
}
