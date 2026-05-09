/* 相册页面 — 三文件夹列表（照片/录像/AI生成）
 *
 * 页面风格与设置页面一致：壁纸 + 半透明遮罩 + 状态栏。
 * 选中效果与主菜单一致：50px/60px 图标大小切换 + 闪烁。
 */
#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "lvgl/lvgl.h"
#include "stream_frame.h"
#include "lvgl_ui.h"
#include "ui_manager.h"
#include "ui_album.h"
#include "ui_album_grid.h"
#include "app_key.h"
#include "../res/res_icons.h"
#include "../app_power.h"
#include "../app_wifi_config.h"

extern lv_font_t lv_font_cn_20_4bpp;
extern lv_font_t lv_font_cn_16_4bpp;
#define UI_TEXT_FONT  &lv_font_cn_20_4bpp
#define UI_SMALL_FONT &lv_font_cn_16_4bpp

#if FS_EN
#include "fatfs/ff.h"

/* ========== 布局常量 ========== */

#define FOLDER_COUNT    3
#define STATUS_BAR_H   26
#define ROW_H           76
#define ROW_GAP         8
#define ICON_SIZE_BIG   60
#define ICON_SIZE_NRM   50
#define FIRST_ROW_Y     (STATUS_BAR_H + 24)
#define BLINK_PERIOD    400

#define COLOR_FONT_BLACK  lv_color_hex(0x000000)
#define COLOR_FONT_GRAY   lv_color_hex(0x666666)

#define BAT_LEVELS     5
static const lv_img_dsc_t *bat_icons[BAT_LEVELS] = {
    &icon_bat0, &icon_bat1, &icon_bat2, &icon_bat3, &icon_bat4
};

/* ========== 文件夹定义 ========== */

typedef struct {
    const char *name;
    const char *sd_path;
    uint16_t   file_count;
} album_folder_t;

static album_folder_t folders[FOLDER_COUNT] = {
    { "\xe7\x85\xa7\xe7\x89\x87",           "0:/photo",   0 },  /* 照片 */
    { "\xe5\xbd\x95\xe5\x83\x8f",           "0:/video",   0 },  /* 录像 */
    { "AI\xe7\x94\x9f\xe6\x88\x90",         "0:/ai_gen",  0 },  /* AI生成 */
};

/* ========== 模块状态 ========== */

static int g_focus_idx = 0;
static uint8_t g_blink_state = 0;
static lv_timer_t *g_blink_timer = NULL;

static lv_obj_t *g_icons[FOLDER_COUNT];
static lv_obj_t *g_name_labels[FOLDER_COUNT];
static lv_obj_t *g_count_labels[FOLDER_COUNT];

/* 状态栏 */
static lv_obj_t *status_time_label;
static lv_obj_t *status_bat_img;
static lv_obj_t *status_wifi_img;

/* ========== 状态栏 ========== */

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

/* ========== 目录文件计数 ========== */

static uint16_t count_files_in_dir(const char *path)
{
    DIR dir;
    FILINFO fno;
    uint16_t count = 0;
    FRESULT res;

    res = f_opendir(&dir, path);
    if (res != FR_OK) {
        os_printf("[album] cannot open %s (res=%d)\r\n", path, res);
        return 0;
    }

    while (1) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == '\0')
            break;
        if (!(fno.fattrib & AM_DIR))
            count++;
    }

    f_closedir(&dir);
    return count;
}

static void scan_all_dirs(void)
{
    for (int i = 0; i < FOLDER_COUNT; i++) {
        folders[i].file_count = count_files_in_dir(folders[i].sd_path);
        os_printf("[album] %s: %d files\r\n", folders[i].sd_path, folders[i].file_count);
    }
}

/* ========== 选中效果：图标大小切换 + 闪烁 ========== */

static void apply_icon_size(int idx, int big)
{
    if (!g_icons[idx]) return;
    lv_img_set_src(g_icons[idx], big ? &folder_icon_60 : &folder_icon_50);
}

static void update_focus(int new_idx)
{
    if (new_idx < 0) new_idx = FOLDER_COUNT - 1;
    if (new_idx >= FOLDER_COUNT) new_idx = 0;

    /* 恢复旧焦点为小图标 */
    apply_icon_size(g_focus_idx, 0);
    g_focus_idx = new_idx;
    g_blink_state = 1;
    apply_icon_size(g_focus_idx, 1);
}

static void blink_cb(lv_timer_t *timer)
{
    g_blink_state = !g_blink_state;
    apply_icon_size(g_focus_idx, g_blink_state);
}

/* ========== 页面回调 ========== */

static lv_obj_t *album_create(ui_page_t *page, lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* 壁纸背景（与主菜单/设置一致） */
    lv_obj_t *bg = lv_img_create(root);
    lv_img_set_src(bg, &Wallpaper0);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 半透明白色遮罩 */
    lv_obj_t *overlay = lv_obj_create(root);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_30, 0);

    /* 状态栏 */
    create_status_bar(overlay);

    /* 标题 "相册" */
    lv_obj_t *title = lv_label_create(overlay);
    lv_label_set_text(title, "\xe7\x9b\xb8\xe5\x86\x8c");  /* 相册 */
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUS_BAR_H + 4);
    lv_obj_set_style_text_color(title, COLOR_FONT_GRAY, 0);
    lv_obj_set_style_text_font(title, UI_TEXT_FONT, 0);

    /* 扫描 SD 卡 */
    scan_all_dirs();

    os_memset(g_icons, 0, sizeof(g_icons));
    os_memset(g_name_labels, 0, sizeof(g_name_labels));
    os_memset(g_count_labels, 0, sizeof(g_count_labels));

    /* 创建 3 行文件夹 */
    for (int i = 0; i < FOLDER_COUNT; i++) {
        int y = FIRST_ROW_Y + i * (ROW_H + ROW_GAP);

        /* 文件夹图标（默认小图标） */
        lv_obj_t *icon = lv_img_create(overlay);
        lv_img_set_src(icon, &folder_icon_50);
        lv_obj_set_pos(icon, 16, y + (ROW_H - ICON_SIZE_NRM) / 2);
        g_icons[i] = icon;

        /* 文件夹名称 */
        lv_obj_t *name = lv_label_create(overlay);
        lv_label_set_text(name, folders[i].name);
        lv_obj_set_pos(name, ICON_SIZE_BIG + 22, y + 14);
        lv_obj_set_style_text_color(name, COLOR_FONT_BLACK, 0);
        lv_obj_set_style_text_font(name, UI_SMALL_FONT, 0);
        g_name_labels[i] = name;

        /* 文件计数 */
        char count_str[32];
        os_sprintf(count_str, "(%d)", folders[i].file_count);
        lv_obj_t *cnt = lv_label_create(overlay);
        lv_label_set_text(cnt, count_str);
        lv_obj_set_pos(cnt, ICON_SIZE_BIG + 22, y + 38);
        lv_obj_set_style_text_color(cnt, COLOR_FONT_GRAY, 0);
        lv_obj_set_style_text_font(cnt, UI_SMALL_FONT, 0);
        g_count_labels[i] = cnt;
    }

    /* 默认选中第一行（大图标） */
    g_focus_idx = 0;
    g_blink_state = 1;
    apply_icon_size(0, 1);

    /* 闪烁定时器 */
    g_blink_timer = lv_timer_create(blink_cb, BLINK_PERIOD, NULL);
    lv_timer_set_repeat_count(g_blink_timer, -1);

    /* Dummy focusable object for LVGL group */
    lv_obj_t *dummy = lv_obj_create(overlay);
    lv_obj_remove_style_all(dummy);
    lv_obj_set_size(dummy, 1, 1);
    lv_obj_set_pos(dummy, 0, 0);
    lv_obj_add_flag(dummy, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_add_obj(ui_manager_group(), dummy);
    lv_group_focus_obj(dummy);

    os_printf("[album] page created\r\n");
    return root;
}

static void album_destroy(ui_page_t *page)
{
    if (g_blink_timer) {
        lv_timer_del(g_blink_timer);
        g_blink_timer = NULL;
    }
    status_bat_img = NULL;
    status_time_label = NULL;
    status_wifi_img = NULL;
    for (int i = 0; i < FOLDER_COUNT; i++) {
        g_icons[i] = NULL;
        g_name_labels[i] = NULL;
        g_count_labels[i] = NULL;
    }
    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
    os_printf("[album] page destroyed\r\n");
}

static void album_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    if (evt != KEY_EVT_SHORT) return;

    if (id == KEY_ID_DOWN) {
        update_focus(g_focus_idx + 1);
        return;
    }
    if (id == KEY_ID_UP) {
        update_focus(g_focus_idx - 1);
        return;
    }
    if (id == KEY_ID_OK) {
        os_printf("[album] enter folder: %s\r\n", folders[g_focus_idx].sd_path);
        ui_album_grid_set_folder(folders[g_focus_idx].sd_path);
        ui_manager_push(PAGE_ALBUM_GRID);
        return;
    }
    if (id == KEY_ID_M) {
        ui_manager_pop();
        return;
    }
}

/* ========== 页面实例 ========== */

static ui_page_t album_page = {
    PAGE_ALBUM,
    album_create,
    album_destroy,
    album_on_key,
    NULL
};

void ui_album_register(void)
{
    ui_page_register(&album_page);
}

#else /* FS_EN == 0 */

static lv_obj_t *album_create(ui_page_t *page, lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    lv_obj_t *lbl = lv_label_create(root);
    lv_label_set_text(lbl, "\xe6\x96\x87\xe4\xbb\xb6\xe7\xb3\xbb\xe7\xbb\x9f\xe6\x9c\xaa\xe5\x90\xaf\xe7\x94\xa8");
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(lbl, COLOR_FONT_GRAY, 0);

    lv_obj_t *dummy = lv_obj_create(root);
    lv_obj_remove_style_all(dummy);
    lv_obj_set_size(dummy, 1, 1);
    lv_obj_set_pos(dummy, 0, 0);
    lv_obj_add_flag(dummy, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_add_obj(ui_manager_group(), dummy);
    lv_group_focus_obj(dummy);

    return root;
}

static void album_destroy(ui_page_t *page)
{
    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
}

static void album_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    if (evt == KEY_EVT_SHORT && (id == KEY_ID_M || id == KEY_ID_OK))
        ui_manager_pop();
}

static ui_page_t album_page = {
    PAGE_ALBUM,
    album_create,
    album_destroy,
    album_on_key,
    NULL
};

void ui_album_register(void)
{
    ui_page_register(&album_page);
}

#endif /* FS_EN */
