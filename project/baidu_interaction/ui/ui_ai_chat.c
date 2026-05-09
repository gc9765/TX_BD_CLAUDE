#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "lvgl/lvgl.h"
#include "ui_manager.h"
#include "app_key.h"
#include "../res/res_icons.h"
#include "../app_power.h"
#include "../app_wifi_config.h"

extern lv_font_t lv_font_cn_16_4bpp;
extern lv_font_t lv_font_cn_18_4bpp;
#define HINT_FONT  &lv_font_cn_16_4bpp
#define LABEL_FONT &lv_font_cn_18_4bpp

#define STATUS_BAR_H  26
#define CHAR_COUNT    4

#define COLOR_BG_BLUE       lv_color_hex(0x1890FF)
#define COLOR_FONT_GRAY     lv_color_hex(0x666666)
#define COLOR_FONT_BLACK    lv_color_hex(0x000000)
#define COLOR_FONT_WHITE    lv_color_hex(0xFFFFFF)

/* BRTC 接口 */
extern void brtc_ptt_start(void);
extern void brtc_ptt_stop(void);
extern void brtc_interrupt_tts(void);
extern void brtc_switch_scene_role(const char *scene_role);

/* ========== 角色数据 ========== */

static const struct {
    const lv_img_dsc_t *icon_110;
    const lv_img_dsc_t *icon_200;
    const char *scene_role;
} g_characters[CHAR_COUNT] = {
    { &chinesepeople1_110,  &chinesepeople1_200,  "top scorer"     },
    { &Doctor1_110,         &Doctor1_200,         "DrOmnipotent"   },
    { &friendship_day_110,  &friendship_day_200,  "Good friend"    },
    { &smart_robot_110,     &smart_robot_200,     "robot"          },
};

/* ========== 状态机 ========== */

enum {
    CHAT_STATE_CHAR_SELECT = 0,
    CHAT_STATE_VOICE       = 1,
};

static uint8_t g_chat_state = CHAT_STATE_CHAR_SELECT;
static uint8_t g_char_idx   = 0;

/* ========== LVGL 对象 ========== */

static lv_obj_t *g_root       = NULL;
static lv_obj_t *g_wallpaper  = NULL;
static lv_obj_t *g_overlay    = NULL;
static lv_obj_t *g_status_bar = NULL;
static lv_obj_t *g_bat_img    = NULL;
static lv_obj_t *g_wifi_img   = NULL;
static lv_obj_t *g_time_lbl   = NULL;

/* 角色选择 */
static lv_obj_t *g_char_img   = NULL;
static lv_obj_t *g_arrow_l    = NULL;
static lv_obj_t *g_arrow_r    = NULL;
static lv_obj_t *g_char_hint  = NULL;

/* 语音对话 */
static lv_obj_t *g_voice_img  = NULL;
static lv_obj_t *g_bottom_bar = NULL;
static lv_obj_t *g_voice_label = NULL;
static lv_obj_t *g_voice_hint  = NULL;

#define BAT_LEVELS 5
static const lv_img_dsc_t *bat_icons[BAT_LEVELS] = {
    &icon_bat0, &icon_bat1, &icon_bat2, &icon_bat3, &icon_bat4
};

/* ========== Status Bar ========== */

static void chat_create_status_bar(lv_obj_t *parent)
{
    g_status_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(g_status_bar);
    lv_obj_set_size(g_status_bar, LV_PCT(100), STATUS_BAR_H);
    lv_obj_set_pos(g_status_bar, 0, 0);
    lv_obj_clear_flag(g_status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(g_status_bar, LV_OPA_30, 0);

    g_bat_img = lv_img_create(g_status_bar);
    {
        enum bat_level lvl = app_battery_get_level();
        lv_img_set_src(g_bat_img, (lvl >= 0 && lvl < BAT_LEVELS) ? bat_icons[lvl] : bat_icons[BAT_LEVEL_HIGH]);
    }
    lv_obj_align(g_bat_img, LV_ALIGN_LEFT_MID, 2, 0);

    g_time_lbl = lv_label_create(g_status_bar);
    lv_label_set_text(g_time_lbl, "00:00");
    lv_obj_align(g_time_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(g_time_lbl, lv_color_hex(0x333333), 0);

    g_wifi_img = lv_img_create(g_status_bar);
    lv_img_set_src(g_wifi_img, g_wifi_connected ? &icon_wifi : &icon_no_wifi);
    lv_obj_align(g_wifi_img, LV_ALIGN_RIGHT_MID, -4, 0);

    ui_manager_set_bat_img(g_bat_img);
    ui_manager_set_wifi_img(g_wifi_img);
    ui_manager_set_time_label(g_time_lbl);
}

/* ========== 角色选择 UI ========== */

static void char_update_arrows(void)
{
    if (g_arrow_l) {
        if (g_char_idx == 0)
            lv_obj_add_flag(g_arrow_l, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(g_arrow_l, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_arrow_r) {
        if (g_char_idx == CHAR_COUNT - 1)
            lv_obj_add_flag(g_arrow_r, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(g_arrow_r, LV_OBJ_FLAG_HIDDEN);
    }
}

static void char_update_icon(void)
{
    if (g_char_img)
        lv_img_set_src(g_char_img, g_characters[g_char_idx].icon_110);
    char_update_arrows();
}

static void show_char_select(void)
{
    g_chat_state = CHAT_STATE_CHAR_SELECT;

    if (g_voice_img)    lv_obj_add_flag(g_voice_img, LV_OBJ_FLAG_HIDDEN);
    if (g_bottom_bar)   lv_obj_add_flag(g_bottom_bar, LV_OBJ_FLAG_HIDDEN);

    if (g_wallpaper)    lv_obj_clear_flag(g_wallpaper, LV_OBJ_FLAG_HIDDEN);
    if (g_overlay)      lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    if (g_status_bar)   lv_obj_clear_flag(g_status_bar, LV_OBJ_FLAG_HIDDEN);
    if (g_char_img)     lv_obj_clear_flag(g_char_img, LV_OBJ_FLAG_HIDDEN);
    if (g_char_hint)    lv_obj_clear_flag(g_char_hint, LV_OBJ_FLAG_HIDDEN);

    char_update_icon();
}

static void show_voice(void)
{
    g_chat_state = CHAT_STATE_VOICE;

    if (g_char_img)     lv_obj_add_flag(g_char_img, LV_OBJ_FLAG_HIDDEN);
    if (g_arrow_l)      lv_obj_add_flag(g_arrow_l, LV_OBJ_FLAG_HIDDEN);
    if (g_arrow_r)      lv_obj_add_flag(g_arrow_r, LV_OBJ_FLAG_HIDDEN);
    if (g_char_hint)    lv_obj_add_flag(g_char_hint, LV_OBJ_FLAG_HIDDEN);

    if (g_voice_img) {
        lv_img_set_src(g_voice_img, g_characters[g_char_idx].icon_200);
        lv_obj_clear_flag(g_voice_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_bottom_bar)   lv_obj_clear_flag(g_bottom_bar, LV_OBJ_FLAG_HIDDEN);
    if (g_voice_label)
        lv_label_set_text(g_voice_label, "AI\xe9\x94\xae\xe8\xaf\xb4\xe8\xaf\x9d"); /* 长按AI键说话 */
    if (g_voice_hint)
        lv_label_set_text(g_voice_hint, "M\xe8\xbf\x94\xe5\x9b\x9e"); /* M返回 */
}

/* ========== 页面生命周期 ========== */

static lv_obj_t *chat_create(ui_page_t *page, lv_obj_t *parent)
{
    g_root = lv_obj_create(parent);
    lv_obj_remove_style_all(g_root);
    lv_obj_set_size(g_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);

    /* 壁纸背景 */
    g_wallpaper = lv_img_create(g_root);
    lv_img_set_src(g_wallpaper, &Wallpaper0);
    lv_obj_set_pos(g_wallpaper, 0, 0);
    lv_obj_clear_flag(g_wallpaper, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 半透明覆盖层 */
    g_overlay = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_overlay);
    lv_obj_set_size(g_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_overlay, 0, 0);
    lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_30, 0);

    chat_create_status_bar(g_overlay);

    /* 角色图标 110x110 居中 */
    int icon_y = STATUS_BAR_H + 60;
    int icon_x = (240 - 110) / 2;

    g_char_img = lv_img_create(g_overlay);
    lv_img_set_src(g_char_img, g_characters[0].icon_110);
    lv_obj_set_pos(g_char_img, icon_x, icon_y);
    lv_obj_clear_flag(g_char_img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 左箭头（next_40x40 旋转 180°） */
    g_arrow_l = lv_img_create(g_overlay);
    lv_img_set_src(g_arrow_l, &next_40x40);
    lv_img_set_angle(g_arrow_l, 1800);
    lv_obj_set_pos(g_arrow_l, 5, icon_y + 35);
    lv_obj_clear_flag(g_arrow_l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 右箭头 */
    g_arrow_r = lv_img_create(g_overlay);
    lv_img_set_src(g_arrow_r, &next_40x40);
    lv_obj_set_pos(g_arrow_r, 195, icon_y + 35);
    lv_obj_clear_flag(g_arrow_r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 底部提示 */
    g_char_hint = lv_label_create(g_overlay);
    lv_label_set_text(g_char_hint, "UP/DOWN\xe6\xb5\x8f\xe8\xa7\x88 OK\xe7\xa1\xae\xe8\xae\xa4"); /* UP/DOWN切换 OK确认 */
    lv_obj_align(g_char_hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_text_color(g_char_hint, COLOR_FONT_GRAY, 0);
    lv_obj_set_style_text_font(g_char_hint, HINT_FONT, 0);

    /* ===== 语音对话 UI（初始隐藏） ===== */

    /* 角色 200px 图标 */
    g_voice_img = lv_img_create(g_overlay);
    lv_img_set_src(g_voice_img, g_characters[0].icon_200);
    lv_obj_align(g_voice_img, LV_ALIGN_TOP_MID, 0, STATUS_BAR_H + 20);
    lv_obj_clear_flag(g_voice_img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_voice_img, LV_OBJ_FLAG_HIDDEN);

    /* 底部栏 */
    g_bottom_bar = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_bottom_bar);
    lv_obj_set_size(g_bottom_bar, LV_PCT(100), 28);
    lv_obj_align(g_bottom_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(g_bottom_bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_bottom_bar, LV_OPA_60, 0);
    lv_obj_clear_flag(g_bottom_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_bottom_bar, LV_OBJ_FLAG_HIDDEN);

    g_voice_label = lv_label_create(g_bottom_bar);
    lv_label_set_text(g_voice_label, "");
    lv_obj_align(g_voice_label, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(g_voice_label, COLOR_FONT_GRAY, 0);
    lv_obj_set_style_text_font(g_voice_label, HINT_FONT, 0);

    g_voice_hint = lv_label_create(g_bottom_bar);
    lv_label_set_text(g_voice_hint, "");
    lv_obj_align(g_voice_hint, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_text_color(g_voice_hint, COLOR_FONT_GRAY, 0);
    lv_obj_set_style_text_font(g_voice_hint, HINT_FONT, 0);

    /* 虚拟焦点对象 */
    lv_obj_t *dummy = lv_obj_create(g_root);
    lv_obj_remove_style_all(dummy);
    lv_obj_set_size(dummy, 1, 1);
    lv_obj_set_pos(dummy, 0, 0);
    lv_obj_add_flag(dummy, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_add_obj(ui_manager_group(), dummy);
    lv_group_focus_obj(dummy);

    g_char_idx = 0;
    show_char_select();

    os_printf("[ai_chat] page created\r\n");
    return g_root;
}

static void chat_destroy(ui_page_t *page)
{
    g_root = NULL;
    g_wallpaper = NULL;
    g_overlay = NULL;
    g_status_bar = NULL;
    g_bat_img = NULL;
    g_wifi_img = NULL;
    g_time_lbl = NULL;
    g_char_img = NULL;
    g_arrow_l = NULL;
    g_arrow_r = NULL;
    g_char_hint = NULL;
    g_voice_img = NULL;
    g_bottom_bar = NULL;
    g_voice_label = NULL;
    g_voice_hint = NULL;

    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
}

static void chat_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    switch (g_chat_state) {

    case CHAT_STATE_CHAR_SELECT:
        if (evt != KEY_EVT_SHORT) return;
        if (id == KEY_ID_UP) {
            g_char_idx = (g_char_idx - 1 + CHAR_COUNT) % CHAR_COUNT;
            char_update_icon();
        } else if (id == KEY_ID_DOWN) {
            g_char_idx = (g_char_idx + 1) % CHAR_COUNT;
            char_update_icon();
        } else if (id == KEY_ID_OK) {
            os_printf("[ai_chat] selected role: %s\r\n", g_characters[g_char_idx].scene_role);
            brtc_switch_scene_role(g_characters[g_char_idx].scene_role);
            show_voice();
        } else if (id == KEY_ID_M) {
            brtc_interrupt_tts();
            ui_manager_pop();
        }
        break;

    case CHAT_STATE_VOICE:
        if (id == KEY_ID_AI) {
            if (evt == KEY_EVT_LONG_START) {
                brtc_ptt_start();
                if (g_voice_label)
                    lv_label_set_text(g_voice_label, "\xe5\xbd\x95\xe9\x9f\xb3\xe4\xb8\xad..."); /* 录音中... */
            } else if (evt == KEY_EVT_LONG_END) {
                brtc_ptt_stop();
                if (g_voice_label)
                    lv_label_set_text(g_voice_label, "AI\xe9\x94\xae\xe8\xaf\xb4\xe8\xaf\x9d"); /* 长按AI键说话 */
            }
            return;
        }
        if (evt != KEY_EVT_SHORT) return;
        if (id == KEY_ID_M) {
            brtc_interrupt_tts();
            show_char_select();
        }
        break;
    }
}

/* ========== 页面注册 ========== */

static ui_page_t ai_chat_page = {
    PAGE_AI_CHAT, chat_create, chat_destroy, chat_on_key, NULL
};

void ui_ai_chat_register(void)
{
    ui_page_register(&ai_chat_page);
}
