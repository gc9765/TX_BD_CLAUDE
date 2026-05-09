#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "lvgl/lvgl.h"
#include "ui_manager.h"
#include "ui_settings.h"
#include "../res/res_icons.h"
#include "../app_wifi_config.h"
#include "../app_power.h"
#include "../app_display.h"
#include "../../syscfg.h"

extern lv_font_t lv_font_cn_20_4bpp;
#define UI_TEXT_FONT &lv_font_cn_20_4bpp

extern lv_font_t lv_font_cn_18_4bpp;
#define UI_SET_FONT &lv_font_cn_18_4bpp

extern lv_font_t lv_font_cn_16_4bpp;
#define UI_HINT_FONT &lv_font_cn_16_4bpp

/* ========== Constants ========== */

#define ITEM_COUNT     5
#define ITEM_H         44
#define STATUS_BAR_H   26
#define FIRST_Y        (STATUS_BAR_H + 50)
#define HIGHLIGHT_W    220
#define HIGHLIGHT_H    (ITEM_H - 6)
#define SCREEN_W       240

/* Colors */
#define COLOR_BG_BLUE     lv_color_hex(0x1890FF)
#define COLOR_FONT_BLACK  lv_color_hex(0x000000)
#define COLOR_FONT_WHITE  lv_color_hex(0xFFFFFF)
#define COLOR_FONT_GRAY   lv_color_hex(0x666666)
#define COLOR_DIVIDER     lv_color_hex(0xCCCCCC)

/* Status bar */
#define BAT_LEVELS     5
static const lv_img_dsc_t *bat_icons[BAT_LEVELS] = {
    &icon_bat0, &icon_bat1, &icon_bat2, &icon_bat3, &icon_bat4
};

/* ========== Settings Items ========== */

typedef struct {
    const char *label;
    const char *value;
    page_id_t   sub_page;
} settings_item_t;

static const settings_item_t items[ITEM_COUNT] = {
    { "\xe8\xaf\xad\xe8\xa8\x80",                    NULL,  PAGE_NONE },          /* 语言 */
    { "\xe9\x9f\xb3\xe9\x87\x8f",                    NULL,  PAGE_NONE },          /* 音量 */
    { "\xe4\xba\xae\xe5\xba\xa6",                    NULL,  PAGE_NONE },          /* 亮度 */
    { "WiFi",                                         NULL,  PAGE_WIFI_SCAN },     /* WiFi */
    { "\xe6\x81\xa2\xe5\xa4\x8d\xe5\x87\xba\xe5\x8e\x82\xe8\xae\xbe\xe7\xbd\xae",  NULL, PAGE_NONE },  /* 恢复出厂设置 */
};

/* ========== Module State ========== */

static lv_obj_t *item_cells[ITEM_COUNT];
static lv_obj_t *item_labels[ITEM_COUNT];
static lv_obj_t *item_values[ITEM_COUNT];
static lv_obj_t *highlight = NULL;
static int focused_idx = 0;

/* Status bar widgets */
static lv_obj_t *status_time_label;
static lv_obj_t *status_bat_img;
static lv_obj_t *status_wifi_img;

/* ========== Submenu (二级菜单) ========== */

#define SUB_MAX 5

static uint8_t  in_submenu = 0;        /* 0=主菜单, 1~3=对应主菜单项 */
static int8_t   sub_sel_idx = 0;
static uint8_t  sub_count = 0;
static lv_obj_t *sub_overlay = NULL;
static lv_obj_t *sub_highlight = NULL;
static lv_obj_t *sub_labels[SUB_MAX];
#define SUB_ITEM_H  36

/* 音量: 静音/低/中/高/最高 → volume_adjust 参数 */
static const char *vol_names[] = {
    "\xe9\x9d\x99\xe9\x9f\xb3",   /* 静音 */
    "\xe4\xbd\x8e",               /* 低 */
    "\xe4\xb8\xad",               /* 中 */
    "\xe9\xab\x98",               /* 高 */
    "\xe6\x9c\x80\xe9\xab\x98",  /* 最高 */
};
static const uint8_t vol_values[] = { 0, 3, 5, 8, 10 };
#define VOL_OPT_COUNT  5
static uint8_t g_volume_idx = 4; /* 默认"最高" */

/* 亮度: 1~5 档 */
static const char *bright_names[] = { "1", "2", "3", "4", "5" };
#define BRIGHT_OPT_COUNT 5
static uint8_t g_brightness_idx = 4; /* 默认第5档 */

/* 语言: 仅中文 */
static const char *lang_names[] = { "\xe4\xb8\xad\xe6\x96\x87" }; /* 中文 */
#define LANG_OPT_COUNT 1

/* Forward declarations */
static void update_highlight(void);
static void update_wifi_text(void);
static void create_status_bar(lv_obj_t *parent);
static void submenu_create(uint8_t main_idx);
static void submenu_destroy(void);
static void submenu_update_highlight(void);
static void update_value_display(int idx);

/* ========== Value display helpers ========== */

static void update_value_display(int idx)
{
    if (!item_values[idx]) return;
    switch (idx) {
    case 0: /* 语言 */
        lv_label_set_text(item_values[idx], lang_names[0]);
        break;
    case 1: /* 音量 */
        lv_label_set_text(item_values[idx], vol_names[g_volume_idx]);
        break;
    case 2: /* 亮度 */
        lv_label_set_text(item_values[idx], bright_names[g_brightness_idx]);
        break;
    case 3: /* WiFi */
        lv_label_set_text(item_values[idx],
            g_wifi_connected ? "\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5"
                             : "\xe6\x9c\xaa\xe8\xbf\x9e\xe6\x8e\xa5");
        break;
    default:
        break;
    }
}

/* ========== Focus / Highlight ========== */

static void focus_changed_cb(lv_group_t *group)
{
    lv_obj_t *focused = lv_group_get_focused(group);
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (item_cells[i] == focused) {
            focused_idx = i;
            break;
        }
    }
    update_highlight();
}

static void update_highlight(void)
{
    if (!highlight) return;

    lv_obj_set_pos(highlight, (SCREEN_W - HIGHLIGHT_W) / 2,
                   FIRST_Y + focused_idx * ITEM_H + 4);

    for (int i = 0; i < ITEM_COUNT; i++) {
        if (item_labels[i]) {
            lv_obj_set_style_text_color(item_labels[i],
                i == focused_idx ? COLOR_FONT_WHITE : COLOR_FONT_BLACK, 0);
        }
        if (item_values[i]) {
            lv_obj_set_style_text_color(item_values[i],
                i == focused_idx ? COLOR_FONT_WHITE : COLOR_FONT_BLACK, 0);
        }
    }
}

static void update_wifi_text(void)
{
    update_value_display(3);
}

/* ========== Status Bar ========== */

static void create_status_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), STATUS_BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(bar, LV_OPA_30, 0);

    status_bat_img = lv_img_create(bar);
    {
        enum bat_level lvl = app_battery_get_level();
        lv_img_set_src(status_bat_img, (lvl >= 0 && lvl < BAT_LEVELS) ? bat_icons[lvl] : bat_icons[BAT_LEVEL_HIGH]);
    }
    lv_obj_align(status_bat_img, LV_ALIGN_LEFT_MID, 2, 0);

    status_time_label = lv_label_create(bar);
    lv_label_set_text(status_time_label, "00:00");
    lv_obj_align(status_time_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(status_time_label, COLOR_FONT_BLACK, 0);

    status_wifi_img = lv_img_create(bar);
    lv_img_set_src(status_wifi_img, g_wifi_connected ? &icon_wifi : &icon_no_wifi);
    lv_obj_align(status_wifi_img, LV_ALIGN_RIGHT_MID, -4, 0);

    ui_manager_set_bat_img(status_bat_img);
    ui_manager_set_wifi_img(status_wifi_img);
    ui_manager_set_time_label(status_time_label);
}

/* ========== Confirm Dialog (Factory Reset) ========== */

static lv_obj_t *dlg_bg = NULL;
static lv_obj_t *dlg_btn_yes = NULL;
static lv_obj_t *dlg_btn_no = NULL;
static int dlg_focus_yes = 0;

static void close_dialog(int yes)
{
    if (dlg_bg) {
        lv_obj_del(dlg_bg);
        dlg_bg = NULL;
    }
    dlg_btn_yes = NULL;
    dlg_btn_no = NULL;
    dlg_focus_yes = 0;

    if (yes) {
        os_printf("[settings] factory reset!\r\n");
        syscfg_set_default_val();
        syscfg_flush(1);
    }

    lv_group_t *grp = ui_manager_group();
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (item_cells[i]) lv_group_add_obj(grp, item_cells[i]);
    }
    lv_group_focus_obj(item_cells[focused_idx]);
    update_highlight();
}

static void show_factory_reset_dialog(lv_obj_t *parent)
{
    lv_group_t *grp = ui_manager_group();
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (item_cells[i]) lv_group_remove_obj(item_cells[i]);
    }

    dlg_bg = lv_obj_create(parent);
    lv_obj_remove_style_all(dlg_bg);
    lv_obj_set_size(dlg_bg, 200, 120);
    lv_obj_center(dlg_bg);
    lv_obj_set_style_bg_color(dlg_bg, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(dlg_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dlg_bg, 10, 0);
    lv_obj_clear_flag(dlg_bg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *msg = lv_label_create(dlg_bg);
    lv_label_set_text(msg, "\xe7\xa1\xae\xe8\xae\xa4\xe6\x81\xa2\xe5\xa4\x8d\xe5\x87\xba\xe5\x8e\x82\xe8\xae\xbe\xe7\xbd\xae\xef\xbc\x9f");
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 15);
    lv_obj_set_style_text_color(msg, COLOR_FONT_BLACK, 0);
    lv_obj_set_style_text_font(msg, UI_SET_FONT, 0);

    dlg_btn_yes = lv_btn_create(dlg_bg);
    lv_obj_set_size(dlg_btn_yes, 70, 30);
    lv_obj_align(dlg_btn_yes, LV_ALIGN_BOTTOM_LEFT, 20, -10);
    lv_obj_set_style_bg_color(dlg_btn_yes, COLOR_BG_BLUE, 0);
    lv_group_add_obj(grp, dlg_btn_yes);

    lv_obj_t *lbl_yes = lv_label_create(dlg_btn_yes);
    lv_label_set_text(lbl_yes, "\xe7\xa1\xae\xe8\xae\xa4");
    lv_obj_center(lbl_yes);
    lv_obj_set_style_text_color(lbl_yes, COLOR_FONT_WHITE, 0);
    lv_obj_set_style_text_font(lbl_yes, UI_SET_FONT, 0);

    dlg_btn_no = lv_btn_create(dlg_bg);
    lv_obj_set_size(dlg_btn_no, 70, 30);
    lv_obj_align(dlg_btn_no, LV_ALIGN_BOTTOM_RIGHT, -20, -10);
    lv_obj_set_style_bg_color(dlg_btn_no, lv_color_hex(0x999999), 0);
    lv_group_add_obj(grp, dlg_btn_no);

    lv_obj_t *lbl_no = lv_label_create(dlg_btn_no);
    lv_label_set_text(lbl_no, "\xe5\x8f\x96\xe6\xb6\x88");
    lv_obj_center(lbl_no);
    lv_obj_set_style_text_color(lbl_no, COLOR_FONT_WHITE, 0);
    lv_obj_set_style_text_font(lbl_no, UI_SET_FONT, 0);

    dlg_focus_yes = 0;
    lv_group_focus_obj(dlg_btn_no);
}

/* ========== Submenu (二级菜单) ========== */

static void submenu_update_highlight(void)
{
    if (!sub_highlight) return;
    lv_obj_set_pos(sub_highlight, 15, 30 + sub_sel_idx * SUB_ITEM_H + 3);
	for (int i = 0; i < sub_count; i++) {
		if (sub_labels[i]) {
			lv_obj_set_style_text_color(sub_labels[i],
			i == sub_sel_idx ? COLOR_FONT_WHITE : COLOR_FONT_BLACK, 0);
        }
    }
}

static void submenu_create(uint8_t main_idx)
{
    in_submenu = main_idx + 1; /* 1-based */

    const char **names = NULL;
    sub_count = 0;
    sub_sel_idx = 0;

    switch (main_idx) {
    case 0: names = lang_names;   sub_count = LANG_OPT_COUNT;   sub_sel_idx = 0; break;
    case 1: names = vol_names;    sub_count = VOL_OPT_COUNT;    sub_sel_idx = g_volume_idx; break;
    case 2: names = bright_names; sub_count = BRIGHT_OPT_COUNT; sub_sel_idx = g_brightness_idx; break;
    default: in_submenu = 0; return;
    }

    int panel_h = 30 + sub_count * SUB_ITEM_H + 10;
    if (panel_h > 260) panel_h = 260;

    sub_overlay = lv_obj_create(lv_obj_get_parent(item_cells[0]));
    lv_obj_remove_style_all(sub_overlay);
    lv_obj_set_size(sub_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(sub_overlay, 0, 0);
    lv_obj_set_style_bg_color(sub_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(sub_overlay, LV_OPA_50, 0);
    lv_obj_clear_flag(sub_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *panel = lv_obj_create(sub_overlay);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 210, panel_h);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    sub_highlight = lv_obj_create(panel);
    lv_obj_remove_style_all(sub_highlight);
    lv_obj_set_size(sub_highlight, 180, SUB_ITEM_H - 6);
    lv_obj_set_pos(sub_highlight, 15, 33);
    lv_obj_set_style_bg_color(sub_highlight, COLOR_BG_BLUE, 0);
    lv_obj_set_style_bg_opa(sub_highlight, LV_OPA_70, 0);
    lv_obj_set_style_radius(sub_highlight, 6, 0);
    lv_obj_set_style_border_width(sub_highlight, 0, 0);
    lv_obj_clear_flag(sub_highlight, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < sub_count && i < SUB_MAX; i++) {
        sub_labels[i] = lv_label_create(panel);
        lv_label_set_text(sub_labels[i], names[i]);
        lv_obj_set_pos(sub_labels[i], 15, 30 + i * SUB_ITEM_H + (SUB_ITEM_H - 12) / 2);
        lv_obj_set_size(sub_labels[i], 180, 14);
        lv_obj_set_style_text_align(sub_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(sub_labels[i], COLOR_FONT_WHITE, 0);
        lv_obj_set_style_text_font(sub_labels[i], UI_SET_FONT, 0);
    }
    for (int i = sub_count; i < SUB_MAX; i++) {
        sub_labels[i] = NULL;
    }

    submenu_update_highlight();
}

static void submenu_apply(void)
{
    uint8_t main_idx = in_submenu - 1;

    switch (main_idx) {
    case 1: /* 音量 */
        g_volume_idx = sub_sel_idx;
        volume_adjust(vol_values[g_volume_idx]);
        update_value_display(1);
        sys_cfgs.user_param1 = (sys_cfgs.user_param1 & 0xFF00) | g_volume_idx;
        syscfg_flush(0);
        os_printf("[settings] volume=%d idx=%d\r\n", vol_values[g_volume_idx], g_volume_idx);
        break;
    case 2: /* 亮度 */
        g_brightness_idx = sub_sel_idx;
        lcd_set_brightness(g_brightness_idx + 1);
        update_value_display(2);
        sys_cfgs.user_param1 = (sys_cfgs.user_param1 & 0x00FF) | (g_brightness_idx << 8);
        syscfg_flush(0);
        os_printf("[settings] brightness=%d\r\n", g_brightness_idx + 1);
        break;
    }
}

static void submenu_destroy(void)
{
    if (sub_overlay) {
        lv_obj_del(sub_overlay);
        sub_overlay = NULL;
    }
    sub_highlight = NULL;
    for (int i = 0; i < SUB_MAX; i++) sub_labels[i] = NULL;
    in_submenu = 0;
}

/* ========== Key Handler (cell callback) ========== */

static void cell_key_handler(lv_event_t *e)
{
    /* 不再通过 cell 事件处理, 全部由 settings_on_key 统一处理 */
}

/* ========== Page Callbacks ========== */

static lv_obj_t *settings_create(ui_page_t *page, lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bg = lv_img_create(root);
    lv_img_set_src(bg, &Wallpaper0);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *overlay = lv_obj_create(root);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_30, 0);

    create_status_bar(overlay);

    lv_obj_t *title = lv_label_create(overlay);
    lv_label_set_text(title, "\xe8\xae\xbe\xe7\xbd\xae");  /* 设置 */
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUS_BAR_H + 18);
    lv_obj_set_style_text_color(title, COLOR_FONT_GRAY, 0);
    lv_obj_set_style_text_font(title, UI_TEXT_FONT, 0);

    /* Highlight bar */
    highlight = lv_obj_create(overlay);
    lv_obj_remove_style_all(highlight);
    lv_obj_set_size(highlight, HIGHLIGHT_W, HIGHLIGHT_H - 8);
    lv_obj_set_pos(highlight, (SCREEN_W - HIGHLIGHT_W) / 2, FIRST_Y + 4);
    lv_obj_set_style_bg_color(highlight, COLOR_BG_BLUE, 0);
    lv_obj_set_style_bg_opa(highlight, LV_OPA_40, 0);
    lv_obj_set_style_radius(highlight, 6, 0);
    lv_obj_set_style_border_width(highlight, 0, 0);
    lv_obj_set_style_shadow_width(highlight, 12, 0);
    lv_obj_set_style_shadow_spread(highlight, 4, 0);
    lv_obj_set_style_shadow_color(highlight, COLOR_BG_BLUE, 0);
    lv_obj_set_style_shadow_opa(highlight, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_x(highlight, 0, 0);
    lv_obj_set_style_shadow_ofs_y(highlight, 0, 0);
    lv_obj_clear_flag(highlight, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Divider lines */
    for (int i = 1; i < ITEM_COUNT; i++) {
        lv_obj_t *line = lv_obj_create(overlay);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, HIGHLIGHT_W - 20, 1);
        lv_obj_set_pos(line, (SCREEN_W - HIGHLIGHT_W + 20) / 2, FIRST_Y + i * ITEM_H);
        lv_obj_set_style_bg_color(line, COLOR_DIVIDER, 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    lv_group_t *grp = ui_manager_group();
    lv_group_set_focus_cb(grp, focus_changed_cb);

    /* Create items */
    for (int i = 0; i < ITEM_COUNT; i++) {
        int y = FIRST_Y + i * ITEM_H;

        lv_obj_t *cell = lv_obj_create(overlay);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, HIGHLIGHT_W, ITEM_H);
        lv_obj_set_pos(cell, (SCREEN_W - HIGHLIGHT_W) / 2, y);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICK_FOCUSABLE);

        lv_obj_t *lbl = lv_label_create(cell);
        lv_label_set_text(lbl, items[i].label);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_set_style_text_color(lbl, COLOR_FONT_BLACK, 0);
        lv_obj_set_style_text_font(lbl, UI_SET_FONT, 0);

        lv_obj_t *val = lv_label_create(cell);
        lv_label_set_text(val, "");
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -20, 0);
        lv_obj_set_style_text_color(val, COLOR_FONT_BLACK, 0);
        lv_obj_set_style_text_font(val, UI_SET_FONT, 0);

        item_cells[i]  = cell;
        item_labels[i] = lbl;
        item_values[i] = val;
        lv_group_add_obj(grp, cell);
    }

    /* Read current brightness/volume state */
    g_brightness_idx = lcd_get_brightness() - 1;
    if (g_brightness_idx >= BRIGHT_OPT_COUNT) g_brightness_idx = 4;
    g_volume_idx = sys_cfgs.user_param1 & 0xFF;
    if (g_volume_idx >= VOL_OPT_COUNT) g_volume_idx = 4;

    for (int i = 0; i < ITEM_COUNT; i++) {
        if (items[i].sub_page == PAGE_NONE) {
            update_value_display(i);
        }
    }
    update_wifi_text();

    focused_idx = 0;
    lv_group_focus_obj(item_cells[0]);
    update_highlight();

    os_printf("[settings] created\r\n");
    return root;
}

static void settings_destroy(ui_page_t *page)
{
    if (sub_overlay) {
        lv_obj_del(sub_overlay);
        sub_overlay = NULL;
    }
    sub_highlight = NULL;
    in_submenu = 0;
    highlight = NULL;
    dlg_bg = NULL;
    dlg_btn_yes = NULL;
    dlg_btn_no = NULL;
    dlg_focus_yes = 0;
    status_bat_img = NULL;
    status_time_label = NULL;
    status_wifi_img = NULL;
    for (int i = 0; i < ITEM_COUNT; i++) {
        item_cells[i]  = NULL;
        item_labels[i] = NULL;
        item_values[i] = NULL;
    }
    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
}

static void settings_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    if (evt != KEY_EVT_SHORT) return;

    /* ---- Submenu active ---- */
    if (in_submenu) {
        if (id == KEY_ID_UP) {
            if (sub_sel_idx > 0) sub_sel_idx--;
            else sub_sel_idx = sub_count - 1;
            submenu_update_highlight();
            return;
        }
        if (id == KEY_ID_DOWN) {
            if (sub_sel_idx < sub_count - 1) sub_sel_idx++;
            else sub_sel_idx = 0;
            submenu_update_highlight();
            return;
        }
        if (id == KEY_ID_OK) {
            submenu_apply();
            submenu_destroy();
            return;
        }
        if (id == KEY_ID_M) {
            submenu_destroy();
            return;
        }
        return;
    }

    /* ---- Dialog active ---- */
    if (dlg_bg) {
        if (id == KEY_ID_M) {
            close_dialog(0);
            return;
        }
        if (id == KEY_ID_UP || id == KEY_ID_DOWN) {
            dlg_focus_yes = !dlg_focus_yes;
            lv_group_focus_obj(dlg_focus_yes ? dlg_btn_yes : dlg_btn_no);
            return;
        }
        if (id == KEY_ID_OK) {
            close_dialog(dlg_focus_yes);
            return;
        }
        return;
    }

    /* ---- Main menu ---- */
    if (id == KEY_ID_M) {
        ui_manager_pop();
        return;
    }
    if (id == KEY_ID_UP) {
        if (focused_idx > 0) focused_idx--;
        else focused_idx = ITEM_COUNT - 1;
        lv_group_focus_obj(item_cells[focused_idx]);
        update_highlight();
        return;
    }
    if (id == KEY_ID_DOWN) {
        if (focused_idx < ITEM_COUNT - 1) focused_idx++;
        else focused_idx = 0;
        lv_group_focus_obj(item_cells[focused_idx]);
        update_highlight();
        return;
    }
    if (id == KEY_ID_OK) {
        os_printf("[settings] OK idx=%d\r\n", focused_idx);
        if (focused_idx == 4) {
            show_factory_reset_dialog(lv_obj_get_parent(item_cells[focused_idx]));
        } else if (focused_idx <= 2) {
            /* 语言/音量/亮度 → 二级菜单 */
            submenu_create(focused_idx);
        } else if (items[focused_idx].sub_page != PAGE_NONE) {
            ui_manager_push(items[focused_idx].sub_page);
        }
        return;
    }
}

/* ========== Page Instance ========== */

ui_page_t settings_list_page = {
    .id       = PAGE_SETTINGS_LIST,
    .create   = settings_create,
    .destroy  = settings_destroy,
    .on_key   = settings_on_key,
    .root_obj = NULL,
};
