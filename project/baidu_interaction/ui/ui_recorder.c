/* 录像机页面 — DVP 全屏预览 + MJPEG AVI 录像
 *
 * 数据流:
 *   DVP摄像头 → VPP(YUV) → preview_encode_vpp(240x320) → R_VIDEO_P0 → LCD
 *                       ↓
 *                  S_JPEG → R_RECORD_JPEG → new_sd_save_avi2 → SD卡 0:/video/
 *
 * OK 键开始/停止录像，M 键返回。录像中显示红点闪烁 + 计时。
 * 预览流复用 ui_camera.c 的 camera_start_streams / camera_stop_streams。
 */
#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "lvgl/lvgl.h"
#include "stream_frame.h"
#include "lvgl_ui.h"
#include "ui_manager.h"
#include "ui_camera.h"
#include "app_key.h"
#include "fatfs/osal_file.h"
#include "video_app/video_app.h"

#if DVP_EN && OPENDML_EN

#include "media.h"
#include "lib/video/dvp/cmos_sensor/csi.h"

/* ========== 录像控制（sd_save.c） ========== */

extern void start_record_thread(uint8_t video_fps, uint8_t audio_frq);
extern uint8_t send_stop_record_cmd(void);
extern uint8_t get_record_thread_status(void);

/* ========== 状态 ========== */

enum {
    REC_STATE_PREVIEW   = 0,
    REC_STATE_RECORDING = 1,
};

static uint8_t  g_rec_state     = REC_STATE_PREVIEW;
static lv_obj_t *g_root         = NULL;
static lv_obj_t *g_rec_dot      = NULL;
static lv_obj_t *g_rec_time_lbl = NULL;
static lv_obj_t *g_bottom_bar   = NULL;
static lv_obj_t *g_hint_lbl     = NULL;
static lv_timer_t *g_rec_timer  = NULL;
static uint32_t g_rec_start_tick = 0;

#define BOTTOM_BAR_H  28
#define HINT_FONT     &lv_font_cn_16_4bpp
extern lv_font_t lv_font_cn_16_4bpp;

/* ========== 录像计时 ========== */

static void rec_timer_cb(lv_timer_t *t)
{
    if (g_rec_state != REC_STATE_RECORDING) return;

    /* 红点闪烁 */
    if (g_rec_dot) {
        if (lv_obj_has_flag(g_rec_dot, LV_OBJ_FLAG_HIDDEN))
            lv_obj_clear_flag(g_rec_dot, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(g_rec_dot, LV_OBJ_FLAG_HIDDEN);
    }

    /* 计时 */
    if (g_rec_time_lbl) {
        uint32_t elapsed = (os_jiffies() - g_rec_start_tick) / 1000;
        uint8_t min = elapsed / 60;
        uint8_t sec = elapsed % 60;
        char buf[8];
        buf[0] = '0' + min / 10;
        buf[1] = '0' + min % 10;
        buf[2] = ':';
        buf[3] = '0' + sec / 10;
        buf[4] = '0' + sec % 10;
        buf[5] = '\0';
        lv_label_set_text(g_rec_time_lbl, buf);
    }
}

static void rec_timer_start(void)
{
    g_rec_start_tick = os_jiffies();
    if (g_rec_dot) lv_obj_clear_flag(g_rec_dot, LV_OBJ_FLAG_HIDDEN);
    if (g_rec_time_lbl) lv_label_set_text(g_rec_time_lbl, "00:00");
    g_rec_timer = lv_timer_create(rec_timer_cb, 500, NULL);
    lv_timer_set_repeat_count(g_rec_timer, -1);
}

static void rec_timer_stop(void)
{
    if (g_rec_timer) {
        lv_timer_del(g_rec_timer);
        g_rec_timer = NULL;
    }
    if (g_rec_dot) lv_obj_add_flag(g_rec_dot, LV_OBJ_FLAG_HIDDEN);
}

/* ========== 录像控制 ========== */

static void stop_recording_and_wait(void)
{
    send_stop_record_cmd();
    int timeout = 200; /* 最多等 2 秒 */
    while (get_record_thread_status() && --timeout > 0)
        os_sleep_ms(10);
    rec_timer_stop();
    g_rec_state = REC_STATE_PREVIEW;
    if (g_hint_lbl)
        lv_label_set_text(g_hint_lbl, "OK\xe5\xbd\x95\xe5\x83\x8f"); /* OK录像 */
}

/* ========== 页面生命周期 ========== */

static lv_obj_t *recorder_create(ui_page_t *page, lv_obj_t *parent)
{
    g_root = lv_obj_create(parent);
    lv_obj_remove_style_all(g_root);
    lv_obj_set_size(g_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);

    /* 防花屏遮罩 */
    camera_create_splash(g_root);

    /* Dummy focusable object */
    lv_obj_t *dummy = lv_obj_create(g_root);
    lv_obj_remove_style_all(dummy);
    lv_obj_set_size(dummy, 1, 1);
    lv_obj_set_pos(dummy, 0, 0);
    lv_obj_add_flag(dummy, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_add_obj(ui_manager_group(), dummy);
    lv_group_focus_obj(dummy);

    /* 录像目录 */
    f_mkdir("0:/video");

    /* 启动预览 */
    camera_start_streams();

    /* ===== 底部栏 ===== */
    g_bottom_bar = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_bottom_bar);
    lv_obj_set_size(g_bottom_bar, LV_PCT(100), BOTTOM_BAR_H);
    lv_obj_align(g_bottom_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(g_bottom_bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_bottom_bar, LV_OPA_60, 0);
    lv_obj_clear_flag(g_bottom_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 红点 (10x10 红色圆) */
    g_rec_dot = lv_obj_create(g_bottom_bar);
    lv_obj_remove_style_all(g_rec_dot);
    lv_obj_set_size(g_rec_dot, 10, 10);
    lv_obj_align(g_rec_dot, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(g_rec_dot, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(g_rec_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_rec_dot, 5, 0);
    lv_obj_clear_flag(g_rec_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_rec_dot, LV_OBJ_FLAG_HIDDEN);

    /* 计时标签 */
    g_rec_time_lbl = lv_label_create(g_bottom_bar);
    lv_label_set_text(g_rec_time_lbl, "");
    lv_obj_align_to(g_rec_time_lbl, g_rec_dot, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    lv_obj_set_style_text_color(g_rec_time_lbl, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_text_font(g_rec_time_lbl, HINT_FONT, 0);

    /* 右侧提示 */
    g_hint_lbl = lv_label_create(g_bottom_bar);
    lv_label_set_text(g_hint_lbl, "OK\xe5\xbd\x95\xe5\x83\x8f"); /* OK录像 */
    lv_obj_align(g_hint_lbl, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_text_color(g_hint_lbl, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(g_hint_lbl, HINT_FONT, 0);

    g_rec_state = REC_STATE_PREVIEW;

    return g_root;
}

static void recorder_destroy(ui_page_t *page)
{
    if (g_rec_state == REC_STATE_RECORDING)
        stop_recording_and_wait();

    camera_cleanup_splash();

    if (g_rec_timer) {
        lv_timer_del(g_rec_timer);
        g_rec_timer = NULL;
    }

    g_root = NULL;
    g_rec_dot = NULL;
    g_rec_time_lbl = NULL;
    g_bottom_bar = NULL;
    g_hint_lbl = NULL;

    camera_stop_streams();

    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
}

static void recorder_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    if (evt != KEY_EVT_SHORT) return;

    if (g_rec_state == REC_STATE_PREVIEW) {
        if (id == KEY_ID_OK) {
            start_record_thread(15, 8);
            g_rec_state = REC_STATE_RECORDING;
            rec_timer_start();
            if (g_hint_lbl)
                lv_label_set_text(g_hint_lbl, "OK\xe5\x81\x9c\xe6\xad\xa2"); /* OK停止 */
            return;
        }
        if (id == KEY_ID_M) {
            ui_manager_pop();
            return;
        }
    }

    if (g_rec_state == REC_STATE_RECORDING) {
        if (id == KEY_ID_OK || id == KEY_ID_M) {
            stop_recording_and_wait();
            if (id == KEY_ID_M)
                ui_manager_pop();
            return;
        }
    }
}

/* ========== 页面注册 ========== */

static ui_page_t recorder_page = {
    PAGE_VIDEO,
    recorder_create,
    recorder_destroy,
    recorder_on_key,
    NULL
};

void ui_recorder_register(void)
{
    ui_page_register(&recorder_page);
}

#else /* DVP_EN == 0 or OPENDML_EN == 0 */

static lv_obj_t *recorder_create(ui_page_t *page, lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    lv_obj_t *lbl = lv_label_create(root);
    lv_label_set_text(lbl, "\xe5\xbd\x95\xe5\x83\x8f\xe6\x9c\xaa\xe5\x90\xaf\xe7\x94\xa8"); /* 录像未启用 */
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x999999), 0);

    lv_obj_t *dummy = lv_obj_create(root);
    lv_obj_remove_style_all(dummy);
    lv_obj_set_size(dummy, 1, 1);
    lv_obj_set_pos(dummy, 0, 0);
    lv_obj_add_flag(dummy, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_add_obj(ui_manager_group(), dummy);
    lv_group_focus_obj(dummy);

    return root;
}

static void recorder_destroy(ui_page_t *page)
{
    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
}

static void recorder_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    if (evt == KEY_EVT_SHORT && (id == KEY_ID_M || id == KEY_ID_OK))
        ui_manager_pop();
}

static ui_page_t recorder_page = {
    PAGE_VIDEO,
    recorder_create,
    recorder_destroy,
    recorder_on_key,
    NULL
};

void ui_recorder_register(void)
{
    ui_page_register(&recorder_page);
}

#endif /* DVP_EN && OPENDML_EN */
