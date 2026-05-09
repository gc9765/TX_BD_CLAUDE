/* 相册 9 宫格文件预览页面
 *
 * 参照参考工程 ui_album_GridPage.c 布局：
 * 3x3 网格，选中蓝色边框，翻页，状态栏。
 * JPEG 缩略图解码使用硬件 JPEG decoder。
 */
#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "lvgl/lvgl.h"
#include "stream_frame.h"
#include "lvgl_ui.h"
#include "ui_manager.h"
#include "ui_album_grid.h"
#include "ui_album_view.h"
#include "app_key.h"
#include "../res/res_icons.h"
#include "../app_power.h"
#include "../app_wifi_config.h"
#include "ui_album_decode.h"
#include "ui_ocr.h"

extern lv_font_t lv_font_cn_20_4bpp;
extern lv_font_t lv_font_cn_16_4bpp;
#define UI_TEXT_FONT  &lv_font_cn_20_4bpp
#define UI_SMALL_FONT &lv_font_cn_16_4bpp
extern lv_font_t lv_font_cn_18_4bpp;
#define UI_MID_FONT   &lv_font_cn_18_4bpp
#include "custom_mem/custom_mem.h"

#if FS_EN

#include "fatfs/ff.h"

/* ========== 布局常量 ========== */

#define GRID_COLS       3
#define GRID_ROWS       3
#define GRID_COUNT      (GRID_COLS * GRID_ROWS)
#define THUMB_W         64
#define THUMB_H         80
#define CELL_GAP_X      10
#define CELL_GAP_Y      6
#define STATUS_BAR_H    26
#define BOTTOM_BAR_H    22
#define GRID_START_Y    (STATUS_BAR_H + 4)

/* 颜色 */
#define COLOR_SELECT_BORDER  0x0078FF
#define COLOR_THUMB_BG       0xCCCCCC
#define COLOR_THUMB_EMPTY    0x444444
#define COLOR_FONT_BLACK     0x000000
#define COLOR_FONT_WHITE     0xFFFFFF
#define COLOR_FONT_GRAY      0x666666

#define MAX_FILES       256
#define FNAME_LEN       64

#define BAT_LEVELS      5
static const lv_img_dsc_t *bat_icons[BAT_LEVELS] = {
    &icon_bat0, &icon_bat1, &icon_bat2, &icon_bat3, &icon_bat4
};

/* ========== 文件列表 ========== */

typedef struct {
    char fname[FNAME_LEN];
} file_entry_t;

/* ========== 模块状态 ========== */

static char g_folder[128];
static file_entry_t *g_files;  /* PSRAM 动态分配 */
static uint16_t g_file_count;
static uint16_t g_cur_page;     /* 0-based */
static uint16_t g_total_pages;
static uint8_t  g_focus;        /* 0-8, 当前页内焦点 */
static uint8_t  g_page_count;   /* 当前页文件数 */

static lv_obj_t *g_cells[GRID_COUNT];
static lv_obj_t *g_borders[GRID_COUNT];
static lv_obj_t *g_labels[GRID_COUNT];
static lv_obj_t *g_page_lbl;
static lv_obj_t *g_title_lbl;
static lv_obj_t *g_empty_lbl;

/* 删除确认弹窗 */
static lv_obj_t *g_root_overlay;
static lv_obj_t *g_dlg_bg;
static lv_obj_t *g_dlg_btn_yes;
static lv_obj_t *g_dlg_btn_no;
static int g_dlg_focus_yes;

/* AI 识别弹窗 */
static lv_obj_t *g_ai_dlg_bg;
static lv_obj_t *g_ai_btn_text;
static lv_obj_t *g_ai_btn_object;
static int g_ai_focus;  /* 0=识字, 1=识物 */

/* 状态栏 */
static lv_obj_t *status_bat_img;
static lv_obj_t *status_time_label;
static lv_obj_t *status_wifi_img;

/* 缩略图 JPEG 解码 */
static uint8_t *g_thumb_buf[GRID_COUNT];      /* PSRAM RGB565 缓冲区 */
static lv_img_dsc_t g_thumb_dsc[GRID_COUNT];  /* LVGL 图像描述符 */
static lv_obj_t *g_thumb_img[GRID_COUNT];     /* LVGL image 控件 */

/* ========== 前向声明 ========== */

static void update_selection(void);
static void load_page(uint16_t page);

/* ========== 目录扫描 ========== */

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t sl = os_strlen(s), xl = os_strlen(suffix);
    if (sl < xl) return 0;
    const char *p = s + sl - xl;
    while (*p && *suffix) {
        char a = *p++, b = *suffix++;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static int is_media_file(const char *fname)
{
    return ends_with_ci(fname, ".jpg") || ends_with_ci(fname, ".jpeg")
        || ends_with_ci(fname, ".avi") || ends_with_ci(fname, ".png");
}

static uint16_t scan_dir(const char *path)
{
    DIR dir;
    FILINFO fno;
    uint16_t cnt = 0;
    if (f_opendir(&dir, path) != FR_OK) return 0;
    while (cnt < MAX_FILES) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') break;
        if (fno.fattrib & AM_DIR) continue;
        if (!is_media_file(fno.fname)) continue;
        os_strncpy(g_files[cnt].fname, fno.fname, FNAME_LEN - 1);
        g_files[cnt].fname[FNAME_LEN - 1] = '\0';
        cnt++;
    }
    f_closedir(&dir);
    return cnt;
}

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
    lv_obj_set_style_text_color(status_time_label, lv_color_hex(COLOR_FONT_BLACK), 0);

    status_wifi_img = lv_img_create(bar);
    lv_img_set_src(status_wifi_img, g_wifi_connected ? &icon_wifi : &icon_no_wifi);
    lv_obj_align(status_wifi_img, LV_ALIGN_RIGHT_MID, -4, 0);

    ui_manager_set_bat_img(status_bat_img);
    ui_manager_set_wifi_img(status_wifi_img);
    ui_manager_set_time_label(status_time_label);
}

/* ========== 选中框 ========== */

static void update_selection(void)
{
    for (int i = 0; i < GRID_COUNT; i++) {
        if (g_borders[i]) {
            if (i == g_focus && i < g_page_count)
                lv_obj_clear_flag(g_borders[i], LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(g_borders[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ========== 加载一页 ========== */

static void load_page(uint16_t page)
{
    if (page >= g_total_pages) page = g_total_pages - 1;
    g_cur_page = page;

    uint16_t start = page * GRID_COUNT;
    g_page_count = (g_file_count > start) ?
        ((g_file_count - start > GRID_COUNT) ? GRID_COUNT : g_file_count - start) : 0;

    for (int i = 0; i < GRID_COUNT; i++) {
        if (i < g_page_count) {
            uint16_t idx = start + i;
            /* 设置文件名标签（解码失败时的后备显示） */
            if (g_labels[i]) {
                char display[FNAME_LEN];
                const char *src = g_files[idx].fname;
                int len = os_strlen(src);
                if (len > 10) {
                    os_memcpy(display, src, 8);
                    os_strcpy(display + 8, "..");
                } else {
                    os_strcpy(display, src);
                }
                lv_label_set_text(g_labels[i], display);
            }
            if (g_cells[i]) {
                lv_obj_clear_flag(g_cells[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(g_cells[i], lv_color_hex(COLOR_THUMB_BG), 0);
            }

            /* 尝试 JPEG / AVI 缩略图解码 */
            if (g_thumb_buf[i] && g_thumb_img[i]) {
                char fullpath[196];
                int dec_ret;
                os_sprintf(fullpath, "%s/%s", g_folder, g_files[idx].fname);
                if (ends_with_ci(g_files[idx].fname, ".avi"))
                    dec_ret = album_decode_avi_thumbnail(fullpath, g_thumb_buf[i], THUMB_W, THUMB_H);
                else
                    dec_ret = album_decode_jpeg_file(fullpath, g_thumb_buf[i], THUMB_W, THUMB_H);
                if (dec_ret == 0) {
                    g_thumb_dsc[i].data = g_thumb_buf[i];
                    lv_img_set_src(g_thumb_img[i], &g_thumb_dsc[i]);
                    lv_obj_clear_flag(g_thumb_img[i], LV_OBJ_FLAG_HIDDEN);
                    if (g_labels[i]) lv_obj_add_flag(g_labels[i], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(g_thumb_img[i], LV_OBJ_FLAG_HIDDEN);
                    if (g_labels[i]) lv_obj_clear_flag(g_labels[i], LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                if (g_labels[i]) lv_obj_clear_flag(g_labels[i], LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            if (g_cells[i]) lv_obj_add_flag(g_cells[i], LV_OBJ_FLAG_HIDDEN);
            if (g_thumb_img[i]) lv_obj_add_flag(g_thumb_img[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 页码 */
    if (g_page_lbl) {
        char buf[16];
        os_sprintf(buf, "%d/%d", g_cur_page + 1, g_total_pages > 0 ? g_total_pages : 1);
        lv_label_set_text(g_page_lbl, buf);
    }

    /* 空提示 */
    if (g_empty_lbl) {
        if (g_file_count == 0)
            lv_obj_clear_flag(g_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(g_empty_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    g_focus = 0;
    update_selection();
}

/* ========== 页面回调 ========== */

static lv_obj_t *grid_create(ui_page_t *page, lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* 壁纸背景 */
    lv_obj_t *bg = lv_img_create(root);
    lv_img_set_src(bg, &Wallpaper0);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 半透明遮罩 */
    lv_obj_t *overlay = lv_obj_create(root);
    g_root_overlay = overlay;
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_30, 0);

    /* 状态栏 */
    create_status_bar(overlay);

    /* 标题 — 文件夹路径（底部左侧） */
    g_title_lbl = lv_label_create(overlay);
    lv_label_set_text(g_title_lbl, g_folder);
    lv_obj_align(g_title_lbl, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_obj_set_style_text_color(g_title_lbl, lv_color_hex(COLOR_FONT_BLACK), 0);
    lv_obj_set_style_text_font(g_title_lbl, UI_SMALL_FONT, 0);

    /* 页码（底部右侧） */
    g_page_lbl = lv_label_create(overlay);
    lv_label_set_text(g_page_lbl, "1/1");
    lv_obj_align(g_page_lbl, LV_ALIGN_BOTTOM_RIGHT, -8, -4);
    lv_obj_set_style_text_color(g_page_lbl, lv_color_hex(COLOR_FONT_BLACK), 0);
    lv_obj_set_style_text_font(g_page_lbl, UI_SMALL_FONT, 0);

    /* 扫描目录（文件列表分配到 PSRAM） */
    if (!g_files) {
        g_files = (file_entry_t *)custom_malloc_psram(MAX_FILES * sizeof(file_entry_t));
    }
    g_file_count = g_files ? scan_dir(g_folder) : 0;
    g_total_pages = (g_file_count + GRID_COUNT - 1) / GRID_COUNT;
    if (g_total_pages == 0) g_total_pages = 1;

    /* 空提示 */
    g_empty_lbl = lv_label_create(overlay);
    lv_label_set_text(g_empty_lbl, "\xe6\x97\xa0\xe7\x85\xa7\xe7\x89\x87"); /* 无照片 */
    lv_obj_align(g_empty_lbl, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_text_color(g_empty_lbl, lv_color_hex(COLOR_FONT_GRAY), 0);
    lv_obj_set_style_text_font(g_empty_lbl, UI_SMALL_FONT, 0);
    lv_obj_add_flag(g_empty_lbl, LV_OBJ_FLAG_HIDDEN);

    /* 计算网格起始 X（居中） */
    int grid_w = GRID_COLS * THUMB_W + (GRID_COLS - 1) * CELL_GAP_X;
    int grid_x = (240 - grid_w) / 2;

    /* 创建 3x3 网格 */
    os_memset(g_cells, 0, sizeof(g_cells));
    os_memset(g_borders, 0, sizeof(g_borders));
    os_memset(g_labels, 0, sizeof(g_labels));

    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            int i = row * GRID_COLS + col;
            int x = grid_x + col * (THUMB_W + CELL_GAP_X);
            int y = GRID_START_Y + row * (THUMB_H + CELL_GAP_Y);

            /* 格子背景 */
            lv_obj_t *cell = lv_obj_create(overlay);
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, THUMB_W, THUMB_H);
            lv_obj_set_pos(cell, x, y);
            lv_obj_set_style_bg_color(cell, lv_color_hex(COLOR_THUMB_BG), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cell, 4, 0);
            lv_obj_set_style_border_color(cell, lv_color_hex(0x666666), 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            g_cells[i] = cell;

            /* 选中框（蓝色边框） */
            lv_obj_t *border = lv_obj_create(overlay);
            lv_obj_remove_style_all(border);
            lv_obj_set_size(border, THUMB_W + 4, THUMB_H + 4);
            lv_obj_set_pos(border, x - 2, y - 2);
            lv_obj_set_style_bg_opa(border, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(border, lv_color_hex(COLOR_SELECT_BORDER), 0);
            lv_obj_set_style_border_width(border, 3, 0);
            lv_obj_set_style_radius(border, 6, 0);
            lv_obj_add_flag(border, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(border, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            g_borders[i] = border;

            /* 文件名标签 */
            lv_obj_t *lbl = lv_label_create(cell);
            lv_label_set_text(lbl, "");
            lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_FONT_BLACK), 0);
            lv_obj_set_style_text_font(lbl, UI_SMALL_FONT, 0);
            g_labels[i] = lbl;

            /* 缩略图 image 控件 */
            lv_obj_t *thumb = lv_img_create(cell);
            lv_obj_align(thumb, LV_ALIGN_CENTER, 0, 0);
            lv_obj_add_flag(thumb, LV_OBJ_FLAG_HIDDEN);
            g_thumb_img[i] = thumb;
        }
    }

    /* 初始化缩略图描述符和 PSRAM 缓冲区 */
    os_memset(g_thumb_buf, 0, sizeof(g_thumb_buf));
    for (int i = 0; i < GRID_COUNT; i++) {
        g_thumb_buf[i] = (uint8_t *)custom_malloc_psram(THUMB_W * THUMB_H * 2);
        if (g_thumb_buf[i]) {
            os_memset(g_thumb_buf[i], 0, THUMB_W * THUMB_H * 2);
        }
        g_thumb_dsc[i].header.cf = LV_IMG_CF_TRUE_COLOR;
        g_thumb_dsc[i].header.w = THUMB_W;
        g_thumb_dsc[i].header.h = THUMB_H;
        g_thumb_dsc[i].data_size = THUMB_W * THUMB_H * 2;
        g_thumb_dsc[i].data = g_thumb_buf[i];
    }

    /* 加载第一页 */
    load_page(0);

    /* Dummy focusable for key handling */
    lv_obj_t *dummy = lv_obj_create(overlay);
    lv_obj_remove_style_all(dummy);
    lv_obj_set_size(dummy, 1, 1);
    lv_obj_set_pos(dummy, 0, 0);
    lv_obj_add_flag(dummy, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_add_obj(ui_manager_group(), dummy);
    lv_group_focus_obj(dummy);

    return root;
}

static void grid_destroy(ui_page_t *page)
{
    g_dlg_bg = NULL;
    g_dlg_btn_yes = NULL;
    g_dlg_btn_no = NULL;
    g_ai_dlg_bg = NULL;
    g_ai_btn_text = NULL;
    g_ai_btn_object = NULL;
    g_root_overlay = NULL;
    status_bat_img = NULL;
    status_time_label = NULL;
    status_wifi_img = NULL;
    g_page_lbl = NULL;
    g_title_lbl = NULL;
    g_empty_lbl = NULL;
    for (int i = 0; i < GRID_COUNT; i++) {
        if (g_thumb_buf[i]) {
            custom_free_psram(g_thumb_buf[i]);
            g_thumb_buf[i] = NULL;
        }
        g_thumb_img[i] = NULL;
        g_cells[i] = NULL;
        g_borders[i] = NULL;
        g_labels[i] = NULL;
    }
    if (g_files) {
        custom_free_psram(g_files);
        g_files = NULL;
    }
    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
}

/* ========== 删除确认弹窗 ========== */

static void grid_close_dialog(int yes)
{
    if (g_dlg_bg) {
        lv_obj_del(g_dlg_bg);
        g_dlg_bg = NULL;
    }
    g_dlg_btn_yes = NULL;
    g_dlg_btn_no = NULL;
    g_dlg_focus_yes = 0;

    if (yes) {
        uint16_t idx = g_cur_page * GRID_COUNT + g_focus;
        if (idx < g_file_count) {
            char fullpath[196];
            os_sprintf(fullpath, "%s/%s", g_folder, g_files[idx].fname);
            f_unlink(fullpath);
            os_printf("[album_grid] deleted %s\r\n", fullpath);
        }
        /* 重新扫描目录 */
        g_file_count = g_files ? scan_dir(g_folder) : 0;
        g_total_pages = (g_file_count + GRID_COUNT - 1) / GRID_COUNT;
        if (g_total_pages == 0) g_total_pages = 1;
        if (g_cur_page >= g_total_pages) g_cur_page = g_total_pages - 1;
        load_page(g_cur_page);
    }
}

static void grid_show_delete_dialog(void)
{
    if (!g_root_overlay) return;

    g_dlg_bg = lv_obj_create(g_root_overlay);
    lv_obj_remove_style_all(g_dlg_bg);
    lv_obj_set_size(g_dlg_bg, 200, 120);
    lv_obj_center(g_dlg_bg);
    lv_obj_set_style_bg_color(g_dlg_bg, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_dlg_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_dlg_bg, 10, 0);
    lv_obj_clear_flag(g_dlg_bg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *msg = lv_label_create(g_dlg_bg);
    lv_label_set_text(msg, "\xe7\xa1\xae\xe8\xae\xa4\xe5\x88\xa0\xe9\x99\xa4?"); /* 确认删除? */
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 15);
    lv_obj_set_style_text_color(msg, lv_color_hex(COLOR_FONT_BLACK), 0);
    lv_obj_set_style_text_font(msg, UI_TEXT_FONT, 0);

    g_dlg_btn_yes = lv_btn_create(g_dlg_bg);
    lv_obj_set_size(g_dlg_btn_yes, 70, 30);
    lv_obj_align(g_dlg_btn_yes, LV_ALIGN_BOTTOM_LEFT, 20, -10);
    lv_obj_set_style_bg_color(g_dlg_btn_yes, lv_color_hex(0xFF4444), 0);
    lv_group_add_obj(ui_manager_group(), g_dlg_btn_yes);

    lv_obj_t *lbl_yes = lv_label_create(g_dlg_btn_yes);
    lv_label_set_text(lbl_yes, "\xe7\xa1\xae\xe8\xae\xa4"); /* 确认 */
    lv_obj_center(lbl_yes);
    lv_obj_set_style_text_color(lbl_yes, lv_color_hex(COLOR_FONT_WHITE), 0);
    lv_obj_set_style_text_font(lbl_yes, UI_TEXT_FONT, 0);

    g_dlg_btn_no = lv_btn_create(g_dlg_bg);
    lv_obj_set_size(g_dlg_btn_no, 70, 30);
    lv_obj_align(g_dlg_btn_no, LV_ALIGN_BOTTOM_RIGHT, -20, -10);
    lv_obj_set_style_bg_color(g_dlg_btn_no, lv_color_hex(0x999999), 0);
    lv_group_add_obj(ui_manager_group(), g_dlg_btn_no);

    lv_obj_t *lbl_no = lv_label_create(g_dlg_btn_no);
    lv_label_set_text(lbl_no, "\xe5\x8f\x96\xe6\xb6\x88"); /* 取消 */
    lv_obj_center(lbl_no);
    lv_obj_set_style_text_color(lbl_no, lv_color_hex(COLOR_FONT_WHITE), 0);
    lv_obj_set_style_text_font(lbl_no, UI_TEXT_FONT, 0);

    g_dlg_focus_yes = 0;
    lv_group_focus_obj(g_dlg_btn_no);
}

/* ========== AI 识别弹窗 ========== */

static void ai_dlg_update_highlight(void)
{
    if (g_ai_btn_text)
        lv_obj_set_style_bg_color(g_ai_btn_text,
            lv_color_hex(g_ai_focus == 0 ? 0x1890FF : 0x999999), 0);
    if (g_ai_btn_object)
        lv_obj_set_style_bg_color(g_ai_btn_object,
            lv_color_hex(g_ai_focus == 1 ? 0x1890FF : 0x999999), 0);
}

static void grid_close_ai_dialog(int choice)
{
    if (g_ai_dlg_bg) {
        lv_obj_del(g_ai_dlg_bg);
        g_ai_dlg_bg = NULL;
    }
    g_ai_btn_text = NULL;
    g_ai_btn_object = NULL;
    g_ai_focus = 0;

    if (choice >= 0 && g_file_count > 0) {
        uint16_t idx = g_cur_page * GRID_COUNT + g_focus;
        if (idx < g_file_count) {
            char fullpath[196];
            os_sprintf(fullpath, "%s/%s", g_folder, g_files[idx].fname);
            uint8_t mode = (choice == 0) ? OCR_MODE_TEXT : OCR_MODE_OBJECT;
            ui_ocr_set_file(fullpath, mode);
            ui_manager_pop();  /* 释放 ALBUM_GRID 栈位，避免 overflow */
            ui_manager_push(choice == 0 ? PAGE_PHOTO_TEXT : PAGE_RECOGNIZE);
        }
    }
}

static void grid_show_ai_dialog(void)
{
    if (!g_root_overlay) return;

    g_ai_dlg_bg = lv_obj_create(g_root_overlay);
    lv_obj_remove_style_all(g_ai_dlg_bg);
    lv_obj_set_size(g_ai_dlg_bg, 200, 120);
    lv_obj_center(g_ai_dlg_bg);
    lv_obj_set_style_bg_color(g_ai_dlg_bg, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_ai_dlg_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_ai_dlg_bg, 10, 0);
    lv_obj_clear_flag(g_ai_dlg_bg, LV_OBJ_FLAG_SCROLLABLE);

    g_ai_btn_text = lv_btn_create(g_ai_dlg_bg);
    lv_obj_set_size(g_ai_btn_text, 160, 34);
    lv_obj_align(g_ai_btn_text, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_bg_color(g_ai_btn_text, lv_color_hex(0x1890FF), 0);
    lv_obj_set_style_radius(g_ai_btn_text, 6, 0);
    lv_group_add_obj(ui_manager_group(), g_ai_btn_text);

    lv_obj_t *lbl_text = lv_label_create(g_ai_btn_text);
    lv_label_set_text(lbl_text, "AI\xe8\xaf\x86\xe5\xad\x97"); /* AI识字 */
    lv_obj_center(lbl_text);
    lv_obj_set_style_text_color(lbl_text, lv_color_hex(COLOR_FONT_WHITE), 0);
    lv_obj_set_style_text_font(lbl_text, UI_TEXT_FONT, 0);

    g_ai_btn_object = lv_btn_create(g_ai_dlg_bg);
    lv_obj_set_size(g_ai_btn_object, 160, 34);
    lv_obj_align(g_ai_btn_object, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_bg_color(g_ai_btn_object, lv_color_hex(0x999999), 0);
    lv_obj_set_style_radius(g_ai_btn_object, 6, 0);
    lv_group_add_obj(ui_manager_group(), g_ai_btn_object);

    lv_obj_t *lbl_obj = lv_label_create(g_ai_btn_object);
    lv_label_set_text(lbl_obj, "AI\xe8\xaf\x86\xe7\x89\xa9"); /* AI识物 */
    lv_obj_center(lbl_obj);
    lv_obj_set_style_text_color(lbl_obj, lv_color_hex(COLOR_FONT_WHITE), 0);
    lv_obj_set_style_text_font(lbl_obj, UI_TEXT_FONT, 0);

    g_ai_focus = 0;
    lv_group_focus_obj(g_ai_btn_text);
}

static void grid_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    /* 删除弹窗激活时拦截所有按键 */
    if (g_dlg_bg) {
        if (evt == KEY_EVT_SHORT) {
            if (id == KEY_ID_M) { grid_close_dialog(0); return; }
            if (id == KEY_ID_UP || id == KEY_ID_DOWN) {
                g_dlg_focus_yes = !g_dlg_focus_yes;
                lv_group_focus_obj(g_dlg_focus_yes ? g_dlg_btn_yes : g_dlg_btn_no);
                return;
            }
            if (id == KEY_ID_OK) { grid_close_dialog(g_dlg_focus_yes); return; }
        }
        return;
    }

    /* AI 弹窗激活时拦截所有按键 */
    if (g_ai_dlg_bg) {
        if (evt == KEY_EVT_SHORT) {
            if (id == KEY_ID_M) { grid_close_ai_dialog(-1); return; }
            if (id == KEY_ID_UP || id == KEY_ID_DOWN) {
                g_ai_focus = !g_ai_focus;
                ai_dlg_update_highlight();
                lv_group_focus_obj(g_ai_focus ? g_ai_btn_object : g_ai_btn_text);
                return;
            }
            if (id == KEY_ID_OK) { grid_close_ai_dialog(g_ai_focus); return; }
        }
        return;
    }

    if (g_file_count == 0) {
        if (evt == KEY_EVT_SHORT && id == KEY_ID_M) ui_manager_pop();
        return;
    }

    /* 长按 OK → 弹出删除确认 */
    if (evt == KEY_EVT_LONG_START && id == KEY_ID_OK) {
        grid_show_delete_dialog();
        return;
    }

    /* 长按 AI → 弹出 AI 识别选项（非视频相册） */
    if (evt == KEY_EVT_LONG_START && id == KEY_ID_AI) {
        if (!os_strstr(g_folder, "video"))
            grid_show_ai_dialog();
        return;
    }

    if (evt != KEY_EVT_SHORT) return;

    switch (id) {
    case KEY_ID_UP:
        if (g_focus > 0) {
            g_focus--;
            update_selection();
        } else if (g_cur_page > 0) {
            load_page(g_cur_page - 1);
            g_focus = g_page_count - 1;
            update_selection();
        }
        break;

    case KEY_ID_DOWN:
        if (g_focus < g_page_count - 1) {
            g_focus++;
            update_selection();
        } else if (g_cur_page < g_total_pages - 1) {
            load_page(g_cur_page + 1);
        }
        break;

    case KEY_ID_OK: {
        uint16_t idx = g_cur_page * GRID_COUNT + g_focus;
        if (idx < g_file_count) {
            ui_album_view_set_file(g_folder, g_files[idx].fname,
                                   idx, g_file_count);
            ui_manager_push(PAGE_ALBUM_VIEW);
        }
        break;
    }

    case KEY_ID_M:
        ui_manager_pop();
        break;

    default:
        break;
    }
}

/* ========== 公共接口 ========== */

void ui_album_grid_set_folder(const char *path)
{
    if (path) {
        os_strncpy(g_folder, path, sizeof(g_folder) - 1);
        g_folder[sizeof(g_folder) - 1] = '\0';
    }
}

static ui_page_t album_grid_page = {
    PAGE_ALBUM_GRID,
    grid_create,
    grid_destroy,
    grid_on_key,
    NULL
};

void ui_album_grid_register(void)
{
    ui_page_register(&album_grid_page);
}

#else /* FS_EN == 0 */

static lv_obj_t *grid_create(ui_page_t *page, lv_obj_t *parent)
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

static void grid_destroy(ui_page_t *page)
{
    if (page->root_obj) { lv_obj_del(page->root_obj); page->root_obj = NULL; }
}

static void grid_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    if (evt == KEY_EVT_SHORT && (id == KEY_ID_M || id == KEY_ID_OK))
        ui_manager_pop();
}

static ui_page_t album_grid_page = {
    PAGE_ALBUM_GRID, grid_create, grid_destroy, grid_on_key, NULL
};

void ui_album_grid_register(void) { ui_page_register(&album_grid_page); }
void ui_album_grid_set_folder(const char *path) { (void)path; }

#endif /* FS_EN */
