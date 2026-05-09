#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "lvgl/lvgl.h"
#include "ui_manager.h"
#include "ui_placeholder.h"
#include "ui_album_decode.h"
#include "ui_main_menu.h"
#include "app_key.h"
#include "custom_mem/custom_mem.h"
#include "../res/res_icons.h"
#include "../app_power.h"
#include "../app_wifi_config.h"
#if FS_EN
#include "fatfs/ff.h"
#endif

extern lv_font_t lv_font_cn_20_4bpp;
extern lv_font_t lv_font_cn_16_4bpp;
extern lv_font_t lv_font_cn_18_4bpp;
#define STYLE_TEXT_FONT &lv_font_cn_20_4bpp
#define HINT_FONT      &lv_font_cn_16_4bpp
#define LABEL_FONT     &lv_font_cn_18_4bpp

/* BRTC 接口（定义在 sdk/app/baidu/src/app_main.c） */
extern void brtc_ptt_start(void);
extern void brtc_ptt_stop(void);
extern void update_prompt_sync(const char *prompt_mode);

/* ========== Status Bar (shared by voice pages) ========== */

#define STATUS_BAR_H   26
#define BAT_LEVELS     5

static const lv_img_dsc_t *bat_icons[BAT_LEVELS] = {
    &icon_bat0, &icon_bat1, &icon_bat2, &icon_bat3, &icon_bat4
};

static lv_obj_t *g_status_bar = NULL;
static lv_obj_t *g_status_bat_img = NULL;
static lv_obj_t *g_status_wifi_img = NULL;
static lv_obj_t *g_status_time_label = NULL;

static void voice_create_status_bar(lv_obj_t *parent)
{
    g_status_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(g_status_bar);
    lv_obj_set_size(g_status_bar, LV_PCT(100), STATUS_BAR_H);
    lv_obj_set_pos(g_status_bar, 0, 0);
    lv_obj_clear_flag(g_status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(g_status_bar, LV_OPA_30, 0);

    g_status_bat_img = lv_img_create(g_status_bar);
    {
        enum bat_level lvl = app_battery_get_level();
        lv_img_set_src(g_status_bat_img, (lvl >= 0 && lvl < BAT_LEVELS) ? bat_icons[lvl] : bat_icons[BAT_LEVEL_HIGH]);
    }
    lv_obj_align(g_status_bat_img, LV_ALIGN_LEFT_MID, 2, 0);

    g_status_time_label = lv_label_create(g_status_bar);
    lv_label_set_text(g_status_time_label, "00:00");
    lv_obj_align(g_status_time_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(g_status_time_label, lv_color_hex(0x333333), 0);

    g_status_wifi_img = lv_img_create(g_status_bar);
    lv_img_set_src(g_status_wifi_img, g_wifi_connected ? &icon_wifi : &icon_no_wifi);
    lv_obj_align(g_status_wifi_img, LV_ALIGN_RIGHT_MID, -4, 0);

    ui_manager_set_bat_img(g_status_bat_img);
    ui_manager_set_wifi_img(g_status_wifi_img);
    ui_manager_set_time_label(g_status_time_label);
}

/* ========== Shared callbacks for all placeholder pages ========== */

static lv_obj_t *placeholder_create(ui_page_t *page, lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "\xe5\x8a\x9f\xe8\x83\xbd\xe5\xbc\x80\xe5\x8f\x91\xe4\xb8\xad"); /* 功能开发中 */
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_text_color(title, lv_color_hex(0x666666), 0);

    lv_obj_t *hint = lv_label_create(root);
    lv_label_set_text(hint, "\xe6\x8c\x89M\xe9\x94\xae\xe8\xbf\x94\xe5\x9b\x9e"); /* 按M键返回 */
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x999999), 0);

    lv_obj_t *dummy = lv_obj_create(root);
    lv_obj_remove_style_all(dummy);
    lv_obj_set_size(dummy, 1, 1);
    lv_obj_set_pos(dummy, 0, 0);
    lv_obj_add_flag(dummy, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_add_obj(ui_manager_group(), dummy);
    lv_group_focus_obj(dummy);

    os_printf("[placeholder] page %d created\r\n", page->id);
    return root;
}

static void placeholder_destroy(ui_page_t *page)
{
    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
}

static void placeholder_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    if (evt != KEY_EVT_SHORT) return;
    if (id == KEY_ID_M || id == KEY_ID_OK) {
        ui_manager_pop();
    }
}

/* ========== 语音交互页面（语音生图 / AI 对话）—— 状态机实现 ========== */

extern uint8_t *voice_gen_get_jpeg(uint32_t *len);
extern void voice_gen_release_jpeg(void);

#define VOICE_IMG_W  240
#define VOICE_IMG_H  320
#define STYLE_COUNT  3
#define ITEM_H       48
#define HIGHLIGHT_W  220
#define HIGHLIGHT_H  (ITEM_H - 6)
#define FIRST_Y      (STATUS_BAR_H + 70)

/* 颜色（参考 ui_settings.c） */
#define COLOR_BG_BLUE       lv_color_hex(0x1890FF)
#define COLOR_BG_BLUE_LIGHT lv_color_hex(0x6DB8FF)
#define COLOR_FONT_BLACK    lv_color_hex(0x000000)
#define COLOR_FONT_WHITE    lv_color_hex(0xFFFFFF)
#define COLOR_FONT_GRAY     lv_color_hex(0x666666)
#define COLOR_DIVIDER       lv_color_hex(0xCCCCCC)

enum {
    VOICE_STATE_STYLE_SELECT  = 0,
    VOICE_STATE_VOICE_INPUT   = 1,
    VOICE_STATE_PREVIEW       = 2,
    VOICE_STATE_SAVE_CONFIRM  = 3,
};

static const struct {
    const char *name;
    const char *mode;
} g_styles[STYLE_COUNT] = {
    { "\xe9\xab\x98\xe6\xb8\x85\xe7\x85\xa7\xe7\x89\x87\xe7\x94\xbb", "3" },  /* 高清照片画 */
    { "\xe5\x8a\xa8\xe6\xbc\xab\xe5\x8d\xa1\xe9\x80\x9a\xe7\x94\xbb", "4" },  /* 动漫卡通画 */
    { "\xe7\xb4\xa0\xe6\x8f\x8f\xe5\x86\x99\xe7\x9c\x9f\xe7\x94\xbb", "5" },  /* 素描写真画 */
};

static uint8_t g_voice_state = VOICE_STATE_STYLE_SELECT;
static int8_t  g_style_idx = 0;

/* 风格选择 UI */
static lv_obj_t *g_style_labels[STYLE_COUNT] = {NULL};
static lv_obj_t *g_style_highlight = NULL;
static lv_obj_t *g_style_title = NULL;
static lv_obj_t *g_style_hint = NULL;
static lv_obj_t *g_style_dividers[STYLE_COUNT - 1] = {NULL};
static lv_obj_t *g_wallpaper = NULL;
static lv_obj_t *g_overlay = NULL;

/* 图像显示 UI */
static lv_obj_t *voice_label = NULL;
static lv_obj_t *voice_hint = NULL;
static lv_timer_t *voice_timer = NULL;
static lv_obj_t *g_voice_img = NULL;
static lv_obj_t *g_bottom_bar = NULL;
static lv_timer_t *g_dot_timer = NULL;
static uint8_t g_dot_count = 0;

static uint8_t *g_voice_rgb_buf = NULL;
static lv_img_dsc_t g_voice_img_dsc;

static uint8_t *g_preview_jpeg = NULL;
static uint32_t g_preview_jpeg_len = 0;

/* 居中生成标签 */
static lv_obj_t *g_center_box = NULL;
static lv_obj_t *g_center_label = NULL;

/* 保存确认弹窗 */
static lv_obj_t *g_confirm_overlay = NULL;
static lv_obj_t *g_confirm_highlight = NULL;
static lv_obj_t *g_confirm_labels[2] = {NULL};
static int8_t g_confirm_idx = 0;

/* ---------- 风格高亮更新 ---------- */

static void style_update_highlight(void)
{
    if (!g_style_highlight) return;
    lv_obj_set_pos(g_style_highlight, (240 - HIGHLIGHT_W) / 2, FIRST_Y + g_style_idx * ITEM_H+ 4);
    for (int i = 0; i < STYLE_COUNT; i++) {
        if (g_style_labels[i]) {
            lv_obj_set_style_text_color(g_style_labels[i],
                i == g_style_idx ? COLOR_FONT_WHITE : COLOR_FONT_BLACK, 0);
        }
    }
}

/* ---------- 文字跳动动画 ---------- */

static void dot_timer_cb(lv_timer_t *t)
{
    g_dot_count = (g_dot_count % 3) + 1;
    if (!g_center_label) return;
    switch (g_dot_count) {
    case 1: lv_label_set_text(g_center_label, "\xe7\x94\x9f\xe6\x88\x90\xe4\xb8\xad.");   break; /* 生成中. */
    case 2: lv_label_set_text(g_center_label, "\xe7\x94\x9f\xe6\x88\x90\xe4\xb8\xad..");  break; /* 生成中.. */
    case 3: lv_label_set_text(g_center_label, "\xe7\x94\x9f\xe6\x88\x90\xe4\xb8\xad..."); break; /* 生成中... */
    }
}

static void dot_anim_start(void)
{
    g_dot_count = 0;
    if (!g_dot_timer)
        g_dot_timer = lv_timer_create(dot_timer_cb, 600, NULL);
    else
        lv_timer_set_period(g_dot_timer, 600);
    dot_timer_cb(NULL);
}

static void dot_anim_stop(void)
{
    if (g_dot_timer) {
        lv_timer_del(g_dot_timer);
        g_dot_timer = NULL;
    }
}

/* ---------- 状态切换 ---------- */

static void voice_show_style_select(void)
{
    g_voice_state = VOICE_STATE_STYLE_SELECT;
    g_style_idx = 0;
    dot_anim_stop();

    /* 显示壁纸 + 覆盖层 + 状态栏 + 风格 UI */
    if (g_wallpaper)      lv_obj_clear_flag(g_wallpaper, LV_OBJ_FLAG_HIDDEN);
    if (g_overlay)        lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    if (g_status_bar)     lv_obj_clear_flag(g_status_bar, LV_OBJ_FLAG_HIDDEN);
    if (g_style_title)    lv_obj_clear_flag(g_style_title, LV_OBJ_FLAG_HIDDEN);
    if (g_style_hint)     lv_obj_clear_flag(g_style_hint, LV_OBJ_FLAG_HIDDEN);
    if (g_style_highlight) lv_obj_clear_flag(g_style_highlight, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < STYLE_COUNT; i++) {
        if (g_style_labels[i]) lv_obj_clear_flag(g_style_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < STYLE_COUNT - 1; i++) {
        if (g_style_dividers[i]) lv_obj_clear_flag(g_style_dividers[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 隐藏图像、底部栏、居中标签、弹窗 */
    if (g_voice_img)         lv_obj_add_flag(g_voice_img, LV_OBJ_FLAG_HIDDEN);
    if (g_bottom_bar)        lv_obj_add_flag(g_bottom_bar, LV_OBJ_FLAG_HIDDEN);
    if (g_center_box)        lv_obj_add_flag(g_center_box, LV_OBJ_FLAG_HIDDEN);
    if (g_confirm_overlay)   lv_obj_add_flag(g_confirm_overlay, LV_OBJ_FLAG_HIDDEN);

    style_update_highlight();

    if (g_preview_jpeg) {
        voice_gen_release_jpeg();
        g_preview_jpeg = NULL;
        g_preview_jpeg_len = 0;
    }
}

static void voice_show_voice_input(void)
{
    g_voice_state = VOICE_STATE_VOICE_INPUT;
    dot_anim_stop();
    if (g_center_box)        lv_obj_add_flag(g_center_box, LV_OBJ_FLAG_HIDDEN);
    if (g_confirm_overlay)   lv_obj_add_flag(g_confirm_overlay, LV_OBJ_FLAG_HIDDEN);

    /* 隐藏风格选择 UI，保留壁纸 + 覆盖层 */
    if (g_style_title)    lv_obj_add_flag(g_style_title, LV_OBJ_FLAG_HIDDEN);
    if (g_style_hint)     lv_obj_add_flag(g_style_hint, LV_OBJ_FLAG_HIDDEN);
    if (g_style_highlight) lv_obj_add_flag(g_style_highlight, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < STYLE_COUNT; i++) {
        if (g_style_labels[i]) lv_obj_add_flag(g_style_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < STYLE_COUNT - 1; i++) {
        if (g_style_dividers[i]) lv_obj_add_flag(g_style_dividers[i], LV_OBJ_FLAG_HIDDEN);
    }

    if (g_bottom_bar) lv_obj_clear_flag(g_bottom_bar, LV_OBJ_FLAG_HIDDEN);
    if (voice_label)
        lv_label_set_text(voice_label, "AI\xe9\x94\xae\xe8\xaf\xb4\xe8\xaf\x9d"); /* AI键说话 */
    if (voice_hint)
        lv_label_set_text(voice_hint, "M\xe8\xbf\x94\xe5\x9b\x9e"); /* M返回 */
    if (g_voice_img) lv_obj_add_flag(g_voice_img, LV_OBJ_FLAG_HIDDEN);
}

static void voice_show_generating(void)
{
    g_voice_state = VOICE_STATE_VOICE_INPUT;
    if (g_bottom_bar)    lv_obj_add_flag(g_bottom_bar, LV_OBJ_FLAG_HIDDEN);
    if (g_center_box)    lv_obj_clear_flag(g_center_box, LV_OBJ_FLAG_HIDDEN);
    dot_anim_start();
}

static void voice_show_preview(void)
{
    g_voice_state = VOICE_STATE_PREVIEW;
    dot_anim_stop();
    if (g_center_box)        lv_obj_add_flag(g_center_box, LV_OBJ_FLAG_HIDDEN);
    if (g_confirm_overlay)   lv_obj_add_flag(g_confirm_overlay, LV_OBJ_FLAG_HIDDEN);

    if (g_voice_img) lv_obj_clear_flag(g_voice_img, LV_OBJ_FLAG_HIDDEN);
    if (g_bottom_bar) lv_obj_clear_flag(g_bottom_bar, LV_OBJ_FLAG_HIDDEN);
    if (voice_label)
        lv_label_set_text(voice_label, "\xe7\x94\x9f\xe6\x88\x90\xe5\xae\x8c\xe6\x88\x90"); /* 生成完成 */
    if (voice_hint)
        lv_label_set_text(voice_hint, "OK\xe4\xbf\x9d\xe5\xad\x98 M\xe8\xbf\x94\xe5\x9b\x9e"); /* OK保存 M返回 */
}

/* ---------- 保存确认弹窗 ---------- */

static void confirm_update_highlight(void)
{
    if (!g_confirm_highlight) return;
    lv_obj_set_pos(g_confirm_highlight, g_confirm_idx == 0 ? 20 : 105, 42);
    for (int i = 0; i < 2; i++) {
        if (g_confirm_labels[i]) {
            lv_obj_set_style_text_color(g_confirm_labels[i],
                i == g_confirm_idx ? COLOR_FONT_WHITE : COLOR_FONT_BLACK, 0);
        }
    }
}

static void voice_show_save_confirm(void)
{
    g_voice_state = VOICE_STATE_SAVE_CONFIRM;
    g_confirm_idx = 0;
    if (g_confirm_overlay) lv_obj_clear_flag(g_confirm_overlay, LV_OBJ_FLAG_HIDDEN);
    confirm_update_highlight();
}

static void voice_hide_save_confirm(void)
{
    if (g_confirm_overlay) lv_obj_add_flag(g_confirm_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- SD 卡保存 ---------- */

#if FS_EN
static void voice_save_jpeg_to_sd(void)
{
    if (!g_preview_jpeg || g_preview_jpeg_len == 0) {
        os_printf("[voice_gen] no JPEG to save\r\n");
        return;
    }

    FRESULT res = f_mkdir("0:/ai_gen");
    if (res == FR_OK) {
        os_printf("[voice_gen] Created ai_gen directory\r\n");
    }

    char filename[64];
    os_sprintf(filename, "0:/ai_gen/AI_%04d.jpg", (uint32_t)(os_jiffies() % 9999));
    os_printf("[voice_gen] saving: %s (%u bytes)\r\n", filename, g_preview_jpeg_len);

    void *fp = osal_fopen(filename, "wb+");
    if (fp) {
        osal_fwrite(g_preview_jpeg, g_preview_jpeg_len, 1, fp);
        osal_fclose(fp);
        os_printf("[voice_gen] saved: %s\r\n", filename);
        if (voice_label)
            lv_label_set_text(voice_label, "\xe4\xbf\x9d\xe5\xad\x98\xe6\x88\x90\xe5\x8a\x9f"); /* 保存成功 */
    } else {
        os_printf("[voice_gen] save failed: %s\r\n", filename);
        if (voice_label)
            lv_label_set_text(voice_label, "\xe4\xbf\x9d\xe5\xad\x98\xe5\xa4\xb1\xe8\xb4\xa5"); /* 保存失败 */
    }
}
#else
static void voice_save_jpeg_to_sd(void) {}
#endif

/* ---------- 轮询生图数据 ---------- */

static void voice_timer_cb(lv_timer_t *t)
{
    if (g_voice_state != VOICE_STATE_VOICE_INPUT) return;

    uint32_t len = 0;
    uint8_t *jpeg = voice_gen_get_jpeg(&len);
    if (jpeg && len > 0) {
        if (g_voice_rgb_buf &&
            img_decode_jpeg_mem(jpeg, len, g_voice_rgb_buf,
                                VOICE_IMG_W, VOICE_IMG_H) == 0) {
            g_voice_img_dsc.data = g_voice_rgb_buf;
            lv_img_set_src(g_voice_img, &g_voice_img_dsc);
        }
        g_preview_jpeg = jpeg;
        g_preview_jpeg_len = len;
        voice_show_preview();
        os_printf("[voice_gen] image displayed on LVGL\r\n");
    }
}

/* ---------- 页面生命周期 ---------- */

static lv_obj_t *voice_create(ui_page_t *page, lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* ===== 壁纸背景（参考 ui_settings.c） ===== */
    g_wallpaper = lv_img_create(root);
    lv_img_set_src(g_wallpaper, &Wallpaper0);
    lv_obj_set_pos(g_wallpaper, 0, 0);
    lv_obj_clear_flag(g_wallpaper, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 半透明白色覆盖层 */
    g_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(g_overlay);
    lv_obj_set_size(g_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_overlay, 0, 0);
    lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_30, 0);

    /* ===== 状态栏（电池/WiFi/时间） ===== */
    voice_create_status_bar(g_overlay);

    /* 标题 */
    g_style_title = lv_label_create(g_overlay);
    lv_label_set_text(g_style_title, "\xe9\x80\x89\xe6\x8b\xa9\xe7\x94\xbb\xe9\xa3\x8e"); /* 选择画风 */
    lv_obj_align(g_style_title, LV_ALIGN_TOP_MID, 0, STATUS_BAR_H + 18);
    lv_obj_set_style_text_color(g_style_title, COLOR_FONT_GRAY, 0);
    lv_obj_set_style_text_font(g_style_title, STYLE_TEXT_FONT, 0);

    /* 蓝色高亮条（shadow 羽化边缘） */
    g_style_highlight = lv_obj_create(g_overlay);
    lv_obj_remove_style_all(g_style_highlight);
    lv_obj_set_size(g_style_highlight, HIGHLIGHT_W, HIGHLIGHT_H - 8);
    lv_obj_set_pos(g_style_highlight, (240 - HIGHLIGHT_W) / 2, FIRST_Y + 4);
    lv_obj_set_style_bg_color(g_style_highlight, COLOR_BG_BLUE, 0);
    lv_obj_set_style_bg_opa(g_style_highlight, LV_OPA_40, 0);
    lv_obj_set_style_radius(g_style_highlight, 6, 0);
	lv_obj_set_style_border_width(g_style_highlight, 0, 0);
    lv_obj_set_style_shadow_width(g_style_highlight, 12, 0);
    lv_obj_set_style_shadow_spread(g_style_highlight, 4, 0);
    lv_obj_set_style_shadow_color(g_style_highlight, COLOR_BG_BLUE, 0);
    lv_obj_set_style_shadow_opa(g_style_highlight, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_x(g_style_highlight, 0, 0);
    lv_obj_set_style_shadow_ofs_y(g_style_highlight, 0, 0);
    lv_obj_clear_flag(g_style_highlight, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 选项间分隔线 */
    for (int i = 1; i < STYLE_COUNT; i++) {
        lv_obj_t *line = lv_obj_create(g_overlay);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, HIGHLIGHT_W - 40, 1);
        lv_obj_set_pos(line, (240 - HIGHLIGHT_W + 40) / 2, FIRST_Y + i * ITEM_H);
        lv_obj_set_style_bg_color(line, COLOR_DIVIDER, 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        g_style_dividers[i - 1] = line;
    }

    /* 3个风格标签 */
    for (int i = 0; i < STYLE_COUNT; i++) {
        g_style_labels[i] = lv_label_create(g_overlay);
        lv_label_set_text(g_style_labels[i], g_styles[i].name);
        lv_obj_align(g_style_labels[i], LV_ALIGN_TOP_MID, 0, FIRST_Y + i * ITEM_H + (HIGHLIGHT_H - 18) / 2);
        lv_obj_set_style_text_font(g_style_labels[i], LABEL_FONT, 0);
    }

    g_style_hint = lv_label_create(g_overlay);
    lv_label_set_text(g_style_hint, "UP/DOWN\xe6\xb5\x8f\xe8\xa7\x88 OK\xe7\xa1\xae\xe8\xae\xa4"); /* UP/DOWN浏览 OK确认 */
    lv_obj_align(g_style_hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_text_color(g_style_hint, COLOR_FONT_GRAY, 0);
    lv_obj_set_style_text_font(g_style_hint, HINT_FONT, 0);

    /* ===== 图像显示 UI（初始隐藏） ===== */
    g_voice_rgb_buf = (uint8_t *)custom_malloc_psram(VOICE_IMG_W * VOICE_IMG_H * 2);
    if (g_voice_rgb_buf) {
        os_memset(g_voice_rgb_buf, 0, VOICE_IMG_W * VOICE_IMG_H * 2);
    }
    g_voice_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    g_voice_img_dsc.header.w = VOICE_IMG_W;
    g_voice_img_dsc.header.h = VOICE_IMG_H;
    g_voice_img_dsc.data_size = VOICE_IMG_W * VOICE_IMG_H * 2;
    g_voice_img_dsc.data = g_voice_rgb_buf;

    g_voice_img = lv_img_create(root);
    lv_img_set_src(g_voice_img, &g_voice_img_dsc);
    lv_obj_align(g_voice_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(g_voice_img, LV_OBJ_FLAG_HIDDEN);

    /* 居中生成文字框（半透明背景） */
    g_center_box = lv_obj_create(root);
    lv_obj_remove_style_all(g_center_box);
    lv_obj_set_size(g_center_box, 100, 44);
    lv_obj_align(g_center_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(g_center_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_center_box, LV_OPA_70, 0);
    lv_obj_set_style_radius(g_center_box, 10, 0);
    lv_obj_clear_flag(g_center_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_center_box, LV_OBJ_FLAG_HIDDEN);

    g_center_label = lv_label_create(g_center_box);
    lv_label_set_text(g_center_label, "");
    lv_obj_align(g_center_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(g_center_label, COLOR_FONT_BLACK, 0);
	lv_obj_set_style_text_font(g_center_label, LABEL_FONT, 0);
	

    /* 底部单行提示栏 */
    g_bottom_bar = lv_obj_create(root);
    lv_obj_remove_style_all(g_bottom_bar);
    lv_obj_set_size(g_bottom_bar, LV_PCT(100), 28);
    lv_obj_align(g_bottom_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(g_bottom_bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_bottom_bar, LV_OPA_60, 0);
    lv_obj_clear_flag(g_bottom_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_bottom_bar, LV_OBJ_FLAG_HIDDEN);

    voice_label = lv_label_create(g_bottom_bar);
    lv_label_set_text(voice_label, "");
    lv_obj_align(voice_label, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(voice_label, COLOR_FONT_GRAY, 0);
	lv_obj_set_style_text_font(voice_label, HINT_FONT, 0);

    voice_hint = lv_label_create(g_bottom_bar);
    lv_label_set_text(voice_hint, "");
    lv_obj_align(voice_hint, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_text_color(voice_hint, COLOR_FONT_GRAY, 0);
    lv_obj_set_style_text_font(voice_hint, HINT_FONT, 0);

    /* ===== 保存确认弹窗（初始隐藏） ===== */
    g_confirm_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(g_confirm_overlay);
    lv_obj_set_size(g_confirm_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_confirm_overlay, 0, 0);
    lv_obj_set_style_bg_color(g_confirm_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_confirm_overlay, LV_OPA_50, 0);
    lv_obj_clear_flag(g_confirm_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_confirm_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *cbox = lv_obj_create(g_confirm_overlay);
    lv_obj_remove_style_all(cbox);
    lv_obj_set_size(cbox, 200, 90);
    lv_obj_align(cbox, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(cbox, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(cbox, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cbox, 10, 0);
    lv_obj_clear_flag(cbox, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ctext = lv_label_create(cbox);
    lv_label_set_text(ctext, "\xe6\x98\xaf\xe5\x90\xa6\xe4\xbf\x9d\xe5\xad\x98\xe5\x88\xb0\xe7\x9b\xb8\xe5\x86\x8c?"); /* 是否保存到相册? */
    lv_obj_align(ctext, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_text_color(ctext, COLOR_FONT_BLACK, 0);
	lv_obj_set_style_text_font(ctext, LABEL_FONT, 0);
	

    g_confirm_highlight = lv_obj_create(cbox);
    lv_obj_remove_style_all(g_confirm_highlight);
    lv_obj_set_size(g_confirm_highlight, 75, 28);
    lv_obj_set_style_bg_color(g_confirm_highlight, COLOR_BG_BLUE, 0);
    lv_obj_set_style_bg_opa(g_confirm_highlight, LV_OPA_80, 0);
    lv_obj_set_style_radius(g_confirm_highlight, 6, 0);
    lv_obj_clear_flag(g_confirm_highlight, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    g_confirm_labels[0] = lv_label_create(cbox);
    lv_label_set_text(g_confirm_labels[0], "\xe6\x98\xaf"); /* 是 */
    lv_obj_set_pos(g_confirm_labels[0], 45, 50);
    lv_obj_set_style_text_color(g_confirm_labels[0], COLOR_FONT_WHITE, 0);
	lv_obj_set_style_text_font(g_confirm_labels[0], LABEL_FONT, 0);

    g_confirm_labels[1] = lv_label_create(cbox);
    lv_label_set_text(g_confirm_labels[1], "\xe5\x90\xa6"); /* 否 */
    lv_obj_set_pos(g_confirm_labels[1], 135, 50);
    lv_obj_set_style_text_color(g_confirm_labels[1], COLOR_FONT_BLACK, 0);
	lv_obj_set_style_text_font(g_confirm_labels[1], LABEL_FONT, 0);

    /* 虚拟可聚焦对象 */
    lv_obj_t *dummy = lv_obj_create(root);
    lv_obj_remove_style_all(dummy);
    lv_obj_set_size(dummy, 1, 1);
    lv_obj_set_pos(dummy, 0, 0);
    lv_obj_add_flag(dummy, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_add_obj(ui_manager_group(), dummy);
    lv_group_focus_obj(dummy);

    voice_timer = lv_timer_create(voice_timer_cb, 20, NULL);

    g_preview_jpeg = NULL;
    g_preview_jpeg_len = 0;
    voice_show_style_select();

    os_printf("[voice_page] page %d created, rgb_buf=%p\r\n", page->id, g_voice_rgb_buf);
    return root;
}

static void voice_destroy(ui_page_t *page)
{
    dot_anim_stop();
    if (voice_timer) {
        lv_timer_del(voice_timer);
        voice_timer = NULL;
    }
    voice_label = NULL;
    voice_hint = NULL;
    g_voice_img = NULL;
    g_center_box = NULL;
    g_center_label = NULL;
    g_bottom_bar = NULL;
    g_confirm_overlay = NULL;
    g_confirm_highlight = NULL;
    for (int i = 0; i < 2; i++)
        g_confirm_labels[i] = NULL;
    g_style_highlight = NULL;
    g_style_title = NULL;
    g_style_hint = NULL;
    g_wallpaper = NULL;
    g_overlay = NULL;
    g_status_bar = NULL;
    g_status_bat_img = NULL;
    g_status_wifi_img = NULL;
    g_status_time_label = NULL;
    for (int i = 0; i < STYLE_COUNT; i++)
        g_style_labels[i] = NULL;
    for (int i = 0; i < STYLE_COUNT - 1; i++)
        g_style_dividers[i] = NULL;
    if (g_voice_rgb_buf) {
        custom_free_psram(g_voice_rgb_buf);
        g_voice_rgb_buf = NULL;
    }
    if (g_preview_jpeg) {
        voice_gen_release_jpeg();
        g_preview_jpeg = NULL;
        g_preview_jpeg_len = 0;
    } else {
        voice_gen_release_jpeg();
    }
    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
}

static void voice_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    switch (g_voice_state) {

    case VOICE_STATE_STYLE_SELECT:
        if (evt != KEY_EVT_SHORT) return;
        if (id == KEY_ID_UP) {
            g_style_idx = (g_style_idx - 1 + STYLE_COUNT) % STYLE_COUNT;
            style_update_highlight();
        } else if (id == KEY_ID_DOWN) {
            g_style_idx = (g_style_idx + 1) % STYLE_COUNT;
            style_update_highlight();
        } else if (id == KEY_ID_OK) {
            os_printf("[voice_gen] style selected: %d (%s)\r\n", g_style_idx, g_styles[g_style_idx].mode);
            update_prompt_sync(g_styles[g_style_idx].mode);
            voice_show_voice_input();
        } else if (id == KEY_ID_M) {
            ui_manager_pop();
        }
        break;

    case VOICE_STATE_VOICE_INPUT:
        if (id == KEY_ID_AI) {
            if (evt == KEY_EVT_LONG_START) {
                brtc_ptt_start();
                dot_anim_stop();
                if (voice_label)
                    lv_label_set_text(voice_label, "\xe5\xbd\x95\xe9\x9f\xb3\xe4\xb8\xad..."); /* 录音中... */
            } else if (evt == KEY_EVT_LONG_END) {
                brtc_ptt_stop();
                voice_show_generating();
            }
            return;
        }
        if (evt != KEY_EVT_SHORT) return;
        if (id == KEY_ID_M) {
            voice_show_style_select();
        }
        break;

    case VOICE_STATE_PREVIEW:
        if (id == KEY_ID_AI) {
            if (evt == KEY_EVT_LONG_START) {
                if (g_preview_jpeg) {
                    voice_gen_release_jpeg();
                    g_preview_jpeg = NULL;
                    g_preview_jpeg_len = 0;
                }
                brtc_ptt_start();
                dot_anim_stop();
                if (voice_label)
                    lv_label_set_text(voice_label, "\xe5\xbd\x95\xe9\x9f\xb3\xe4\xb8\xad..."); /* 录音中... */
            } else if (evt == KEY_EVT_LONG_END) {
                brtc_ptt_stop();
                voice_show_generating();
            }
            return;
        }
        if (evt != KEY_EVT_SHORT) return;
        if (id == KEY_ID_OK) {
            voice_show_save_confirm();
        } else if (id == KEY_ID_M) {
            voice_show_style_select();
        }
        break;

    case VOICE_STATE_SAVE_CONFIRM:
        if (evt != KEY_EVT_SHORT) return;
        if ( id == KEY_ID_UP) {
            g_confirm_idx = 0;
            confirm_update_highlight();
        } else if ( id == KEY_ID_DOWN) {
            g_confirm_idx = 1;
            confirm_update_highlight();
        } else if (id == KEY_ID_OK) {
            voice_hide_save_confirm();
            if (g_confirm_idx == 0) {
                voice_save_jpeg_to_sd();
                if (voice_hint)
                    lv_label_set_text(voice_hint, "M\xe8\xbf\x94\xe5\x9b\x9e"); /* M返回 */
            }
            g_voice_state = VOICE_STATE_PREVIEW;
        } else if (id == KEY_ID_M) {
            voice_hide_save_confirm();
            g_voice_state = VOICE_STATE_PREVIEW;
        }
        break;
    }
}

/* ========== Page instances ========== */

static ui_page_t pages[] = {
    { PAGE_VOICE_GEN,  voice_create,      voice_destroy,      voice_on_key,      NULL },
    { PAGE_PHOTO_TEXT, NULL, NULL, NULL, NULL },
    { PAGE_RECOGNIZE,  NULL, NULL, NULL, NULL },
    { PAGE_AI_CHAT,    NULL, NULL, NULL, NULL },
    { PAGE_CAMERA,     NULL, NULL, NULL, NULL },
    { PAGE_VIDEO,      NULL, NULL, NULL, NULL },
    { PAGE_ALBUM,      NULL, NULL, NULL, NULL },
};

#define PLACEHOLDER_COUNT  (sizeof(pages) / sizeof(pages[0]))

void ui_placeholder_register_all(void)
{
    for (int i = 0; i < (int)PLACEHOLDER_COUNT; i++) {
        if (pages[i].create == NULL) continue;
        ui_page_register(&pages[i]);
    }
}
