
#include "sys_config.h"
#include "typesdef.h"
#include "osal/task.h"
#include "osal/string.h"
#include "printer_key.h"
#include "printer_power.h"
#include "printer_driver.h"
#include "printer_voice.h"
#include "syscfg.h"

/* ========== 按键动作处理 ========== */

static const char * const mode_names[MODE_COUNT] = {
	"sketch_pen", "line_art", "pencil", "anime", "cartoon", "ink_wash"
};

static printer_mode_t g_printer_mode = MODE_SKETCH_PEN;

static void load_printer_mode(void) {
	extern struct sys_config sys_cfgs;
	uint32_t val = sys_cfgs.user_param1;
	if (val < MODE_COUNT) {
		g_printer_mode = (printer_mode_t)val;
	} else {
		g_printer_mode = MODE_SKETCH_PEN;  // 非法值回退默认
	}
		os_printf("[printer_app] mode loaded: %s (%d)\r\n",
		mode_names[g_printer_mode], g_printer_mode);
	}

static void save_printer_mode(void) {
	extern struct sys_config sys_cfgs;
	sys_cfgs.user_param1 = (uint32_t)g_printer_mode;
	syscfg_save();
	os_printf("[printer_app] mode saved: %s (%d)\r\n",
				mode_names[g_printer_mode], g_printer_mode);
}

static void on_key_action(key_action_t action)
{
    switch (action) {
		case KEY_ACT_DOUBLE:
			/* ensure voice playback and stream GC have fully released resources
			 * before syscfg_save() calls malloc internally */
			printer_voice_deinit();
			os_sleep_ms(50);
			save_printer_mode(); //关机前模式保存
			os_printf("[printer_app] double click -> power off\r\n");
			printer_power_off();
			break;

		case KEY_ACT_SHORT:
			g_printer_mode = (g_printer_mode + 1) % MODE_COUNT; //模式切换
			os_printf("[printer_app] short press -> mode: %s\r\n", mode_names[g_printer_mode]);
			printer_voice_announce(g_printer_mode);
			extern void update_prompt_by_printer_mode(printer_mode_t mode);
			update_prompt_by_printer_mode(g_printer_mode);
			break;

		case KEY_ACT_LONG_START:
		{
			/* 长按开始 → 开始录音/检测纸张（后续实现） */
			extern void brtc_ptt_start(void);
			brtc_ptt_start();
	//		printer_action = 1;
			os_printf("[printer_app] long press -> PTT start\r\n");
			break;
		}
		case KEY_ACT_LONG_END:
		{
			/* 长按释放 → 停止录音（后续实现） */
			extern void brtc_ptt_stop(void);
			brtc_ptt_stop();
			os_printf("[printer_app] long press -> PTT stop\r\n");
			break;
		}
    }
}

/* ========== 初始化入口 ========== */

void printer_app_init(void)
{
    /* 1. 电源保持：PA_0 拉高，防止 BY25064A1TG 超时断电 */
    printer_power_init();

    /* 2. 按键检测：注册回调，监听 PA_1 电源按键 */
    printer_key_init(on_key_action);

    /* 3. 启动打印机任务线程 */
    printer_thread_init();
	
	load_printer_mode(); //从 flash 恢复模式
	printer_voice_init();
	printer_voice_announce(g_printer_mode);
    os_printf("[printer_app] init done, power hold ON\r\n");
}

/* 供外部模块读取当前模式 */
printer_mode_t printer_get_mode(void) {
    return g_printer_mode;
}
