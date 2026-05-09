/* 相册大图全屏查看页面
 *
 * 参照参考工程 ui_albumFullscreenPage.c 布局：
 * 黑色背景，顶部电池，底部文件名+计数。
 * 左右键切换文件。
 */
#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "lvgl/lvgl.h"
#include "stream_frame.h"
#include "lvgl_ui.h"
#include "ui_manager.h"
#include "ui_album_view.h"
#include "app_key.h"
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

#define BOTTOM_BAR_H    24
#define FNAME_LEN       64
#define MAX_FILES       256
#define VIEW_IMG_W      240
#define VIEW_IMG_H      296

#define COLOR_FONT_WHITE  0xFFFFFF
#define COLOR_FONT_GRAY   0x999999

/* ========== 文件列表（独立扫描） ========== */

typedef struct {
    char fname[FNAME_LEN];
} view_entry_t;

static view_entry_t *g_view_files;  /* PSRAM 动态分配 */

/* ========== 模块状态 ========== */

static char g_folder[128];
static char g_cur_fname[FNAME_LEN];
static uint16_t g_file_index;
static uint16_t g_total_files;

static lv_obj_t *g_filename_lbl;
static lv_obj_t *g_counter_lbl;
static lv_obj_t *g_center_lbl;    /* 中央占位文字（无文件/解码失败时显示）*/

/* JPEG 解码图像 */
static uint8_t *g_full_buf;               /* PSRAM RGB565 缓冲区 */
static lv_img_dsc_t g_full_dsc;           /* LVGL 图像描述符 */
static lv_obj_t *g_full_img;              /* LVGL image 控件 */

/* AVI 视频播放 */
static FIL       g_avi_fp;
static uint8_t   g_avi_opened;
static uint32_t  g_avi_pos;
static uint32_t  g_avi_fsize;
static uint8_t   g_playing;
static uint8_t   g_paused;
static lv_timer_t *g_play_timer;
static uint8_t  *g_jpg_buf;

#define PLAY_JPG_BUF_SIZE  (40 * 1024)
#define PLAY_FPS           15

/* 删除确认弹窗 */
static lv_obj_t *g_dlg_bg;
static lv_obj_t *g_dlg_btn_yes;
static lv_obj_t *g_dlg_btn_no;
static int g_dlg_focus_yes;
static lv_obj_t *g_view_root;

/* AI 识别弹窗 */
static lv_obj_t *g_ai_dlg_bg;
static lv_obj_t *g_ai_btn_text;
static lv_obj_t *g_ai_btn_object;
static int g_ai_focus;

/* ========== 辅助函数 ========== */

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
        os_strncpy(g_view_files[cnt].fname, fno.fname, FNAME_LEN - 1);
        g_view_files[cnt].fname[FNAME_LEN - 1] = '\0';
        cnt++;
    }
    f_closedir(&dir);
    return cnt;
}

/* ========== AVI 视频播放（仅画面）========== */

static void avi_play_stop(void)
{
    g_playing = 0;
    g_paused = 0;
    if (g_play_timer) {
        lv_timer_del(g_play_timer);
        g_play_timer = NULL;
    }
    if (g_avi_opened) {
        f_close(&g_avi_fp);
        g_avi_opened = 0;
    }
    if (g_jpg_buf) {
        custom_free_psram(g_jpg_buf);
        g_jpg_buf = NULL;
    }
}

static void avi_play_timer_cb(lv_timer_t *t)
{
    uint8_t buf[8];
    uint32_t chunk_size, br;

    if (!g_playing || g_paused || !g_jpg_buf) return;

    while (g_avi_pos + 8 < g_avi_fsize) {
        f_lseek(&g_avi_fp, g_avi_pos);
        if (f_read(&g_avi_fp, buf, 8, &br) != FR_OK || br < 8) {
            avi_play_stop();
            return;
        }

        chunk_size = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24);

        /* 00dc = MJPEG video frame → decode, display, return */
        if (buf[0] == 0x30 && buf[1] == 0x30 &&
            buf[2] == 0x64 && buf[3] == 0x63) {
            g_avi_pos += 8;
            if (chunk_size == 0 || chunk_size > PLAY_JPG_BUF_SIZE) {
                avi_play_stop();
                return;
            }
            f_read(&g_avi_fp, g_jpg_buf, chunk_size, &br);
            g_avi_pos += (chunk_size + 1) & ~1;

            if (br >= 2 && g_jpg_buf[0] == 0xFF && g_jpg_buf[1] == 0xD8 &&
                g_full_buf && g_full_img &&
                img_decode_jpeg_mem(g_jpg_buf, br, g_full_buf,
                                    VIEW_IMG_W, VIEW_IMG_H) == 0) {
                g_full_dsc.data = g_full_buf;
                lv_img_set_src(g_full_img, &g_full_dsc);
                lv_obj_clear_flag(g_full_img, LV_OBJ_FLAG_HIDDEN);
                if (g_center_lbl) lv_obj_add_flag(g_center_lbl, LV_OBJ_FLAG_HIDDEN);
            }
            return;
        }

        /* 01wb = PCM audio → skip */
        if (buf[0] == 0x30 && buf[1] == 0x31 &&
            buf[2] == 0x77 && buf[3] == 0x62) {
            g_avi_pos += 8 + ((chunk_size + 1) & ~1);
            continue;
        }

        /* RIFF / LIST: skip 12-byte header */
        if ((buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F') ||
            (buf[0] == 'L' && buf[1] == 'I' && buf[2] == 'S' && buf[3] == 'T')) {
            g_avi_pos += 12;
            continue;
        }

        /* Other chunks */
        g_avi_pos += 8 + ((chunk_size + 1) & ~1);
    }

    avi_play_stop();
}

static void avi_play_start(const char *filepath)
{
    if (f_open(&g_avi_fp, filepath, FA_READ) != FR_OK)
        return;

    g_avi_fsize = f_size(&g_avi_fp);
    g_avi_pos = 0;
    g_avi_opened = 1;

    g_jpg_buf = (uint8_t *)custom_malloc_psram(PLAY_JPG_BUF_SIZE);
    if (!g_jpg_buf) {
        f_close(&g_avi_fp);
        g_avi_opened = 0;
        return;
    }

    g_playing = 1;
    g_paused = 0;
    g_play_timer = lv_timer_create(avi_play_timer_cb, 1000 / PLAY_FPS, NULL);
    lv_timer_set_repeat_count(g_play_timer, -1);
}

/* ========== 更新显示 ========== */

static void show_current_file(void)
{
    /* 停止之前的 AVI 播放 */
    avi_play_stop();

    if (g_file_index >= g_total_files) {
        if (g_filename_lbl) lv_label_set_text(g_filename_lbl, "");
        if (g_counter_lbl) lv_label_set_text(g_counter_lbl, "0/0");
        if (g_center_lbl)  lv_label_set_text(g_center_lbl, "\xe6\x97\xa0\xe6\x96\x87\xe4\xbb\xb6"); /* 无文件 */
        if (g_full_img) lv_obj_add_flag(g_full_img, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    os_strncpy(g_cur_fname, g_view_files[g_file_index].fname, FNAME_LEN - 1);
    g_cur_fname[FNAME_LEN - 1] = '\0';

    if (g_filename_lbl)
        lv_label_set_text(g_filename_lbl, g_cur_fname);

    if (g_counter_lbl) {
        char buf[16];
        os_sprintf(buf, "%d/%d", g_file_index + 1, g_total_files);
        lv_label_set_text(g_counter_lbl, buf);
    }

    char fullpath[196];
    os_sprintf(fullpath, "%s/%s", g_folder, g_cur_fname);

    /* AVI 视频播放 */
    if (ends_with_ci(g_cur_fname, ".avi")) {
        avi_play_start(fullpath);
        return;
    }

    /* JPEG 解码显示 */
    if (g_full_buf && g_full_img) {
        if (album_decode_jpeg_file(fullpath, g_full_buf, VIEW_IMG_W, VIEW_IMG_H) == 0) {
            g_full_dsc.data = g_full_buf;
            lv_img_set_src(g_full_img, &g_full_dsc);
            lv_obj_clear_flag(g_full_img, LV_OBJ_FLAG_HIDDEN);
            if (g_center_lbl) lv_obj_add_flag(g_center_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_full_img, LV_OBJ_FLAG_HIDDEN);
            if (g_center_lbl) {
                lv_label_set_text(g_center_lbl, g_cur_fname);
                lv_obj_clear_flag(g_center_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        if (g_center_lbl) {
            lv_label_set_text(g_center_lbl, g_cur_fname);
            lv_obj_clear_flag(g_center_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ========== 页面回调 ========== */

static lv_obj_t *view_create(ui_page_t *page, lv_obj_t *parent)
{
    /* 全黑背景 */
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    g_view_root = root;

    /* === 全屏图像区域 === */
    g_full_buf = (uint8_t *)custom_malloc_psram(VIEW_IMG_W * VIEW_IMG_H * 2);
    if (g_full_buf) {
        os_memset(g_full_buf, 0, VIEW_IMG_W * VIEW_IMG_H * 2);
    }
    g_full_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    g_full_dsc.header.w = VIEW_IMG_W;
    g_full_dsc.header.h = VIEW_IMG_H;
    g_full_dsc.data_size = VIEW_IMG_W * VIEW_IMG_H * 2;
    g_full_dsc.data = g_full_buf;

    g_full_img = lv_img_create(root);
    lv_img_set_src(g_full_img, &g_full_dsc);
    lv_obj_align(g_full_img, LV_ALIGN_CENTER, 0, -BOTTOM_BAR_H / 2);
    lv_obj_add_flag(g_full_img, LV_OBJ_FLAG_HIDDEN);

    /* 中央文字（解码失败时后备显示） */
    g_center_lbl = lv_label_create(root);
    lv_label_set_text(g_center_lbl, "");
    lv_obj_align(g_center_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(g_center_lbl, lv_color_hex(COLOR_FONT_GRAY), 0);
    lv_obj_set_style_text_font(g_center_lbl, UI_SMALL_FONT, 0);

    /* === 底部栏：文件名 + 计数 === */
    lv_obj_t *btm = lv_obj_create(root);
    lv_obj_remove_style_all(btm);
    lv_obj_set_size(btm, LV_PCT(100), BOTTOM_BAR_H);
    lv_obj_align(btm, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(btm, LV_OBJ_FLAG_SCROLLABLE);

    g_filename_lbl = lv_label_create(btm);
    lv_label_set_text(g_filename_lbl, "");
    lv_obj_align(g_filename_lbl, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(g_filename_lbl, lv_color_hex(COLOR_FONT_WHITE), 0);
    lv_obj_set_style_text_font(g_filename_lbl, UI_SMALL_FONT, 0);

    g_counter_lbl = lv_label_create(btm);
    lv_label_set_text(g_counter_lbl, "");
    lv_obj_align(g_counter_lbl, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_text_color(g_counter_lbl, lv_color_hex(COLOR_FONT_WHITE), 0);
    lv_obj_set_style_text_font(g_counter_lbl, UI_SMALL_FONT, 0);

    /* 扫描目录（文件列表分配到 PSRAM） */
    if (!g_view_files) {
        g_view_files = (view_entry_t *)custom_malloc_psram(MAX_FILES * sizeof(view_entry_t));
    }
    g_total_files = g_view_files ? scan_dir(g_folder) : 0;

    show_current_file();

    /* Dummy focusable */
    lv_obj_t *dummy = lv_obj_create(root);
    lv_obj_remove_style_all(dummy);
    lv_obj_set_size(dummy, 1, 1);
    lv_obj_set_pos(dummy, 0, 0);
    lv_obj_add_flag(dummy, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_add_obj(ui_manager_group(), dummy);
    lv_group_focus_obj(dummy);

    return root;
}

static void view_destroy(ui_page_t *page)
{
    avi_play_stop();
    g_dlg_bg = NULL;
    g_dlg_btn_yes = NULL;
    g_dlg_btn_no = NULL;
    g_ai_dlg_bg = NULL;
    g_ai_btn_text = NULL;
    g_ai_btn_object = NULL;
    g_view_root = NULL;
    g_filename_lbl = NULL;
    g_counter_lbl = NULL;
    g_center_lbl = NULL;
    g_full_img = NULL;
    if (g_view_files) {
        custom_free_psram(g_view_files);
        g_view_files = NULL;
    }
    if (g_full_buf) {
        custom_free_psram(g_full_buf);
        g_full_buf = NULL;
    }
    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
}

/* ========== 删除确认弹窗 ========== */

static void view_close_dialog(int yes)
{
    if (g_dlg_bg) {
        lv_obj_del(g_dlg_bg);
        g_dlg_bg = NULL;
    }
    g_dlg_btn_yes = NULL;
    g_dlg_btn_no = NULL;
    g_dlg_focus_yes = 0;

    if (yes) {
        char fullpath[196];
        os_sprintf(fullpath, "%s/%s", g_folder, g_cur_fname);
        f_unlink(fullpath);
        os_printf("[album_view] deleted %s\r\n", fullpath);

        /* 重新扫描目录 */
        g_total_files = g_view_files ? scan_dir(g_folder) : 0;
        if (g_file_index >= g_total_files && g_file_index > 0)
            g_file_index--;
        show_current_file();
    }
}

static void view_show_delete_dialog(void)
{
    if (!g_view_root) return;

    g_dlg_bg = lv_obj_create(g_view_root);
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
    lv_obj_set_style_text_color(msg, lv_color_hex(0x000000), 0);
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

static void view_ai_dlg_update_highlight(void)
{
    if (g_ai_btn_text)
        lv_obj_set_style_bg_color(g_ai_btn_text,
            lv_color_hex(g_ai_focus == 0 ? 0x1890FF : 0x999999), 0);
    if (g_ai_btn_object)
        lv_obj_set_style_bg_color(g_ai_btn_object,
            lv_color_hex(g_ai_focus == 1 ? 0x1890FF : 0x999999), 0);
}

static void view_close_ai_dialog(int choice)
{
    if (g_ai_dlg_bg) {
        lv_obj_del(g_ai_dlg_bg);
        g_ai_dlg_bg = NULL;
    }
    g_ai_btn_text = NULL;
    g_ai_btn_object = NULL;
    g_ai_focus = 0;

    if (choice >= 0 && g_file_index < g_total_files) {
        char fullpath[196];
        os_sprintf(fullpath, "%s/%s", g_folder, g_cur_fname);
        uint8_t mode = (choice == 0) ? OCR_MODE_TEXT : OCR_MODE_OBJECT;
        ui_ocr_set_file(fullpath, mode);
        ui_manager_pop();  /* 释放 ALBUM_VIEW 栈位，避免 overflow */
        ui_manager_push(choice == 0 ? PAGE_PHOTO_TEXT : PAGE_RECOGNIZE);
    }
}

static void view_show_ai_dialog(void)
{
    if (!g_view_root) return;

    g_ai_dlg_bg = lv_obj_create(g_view_root);
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

static void view_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    /* 删除弹窗激活时拦截所有按键 */
    if (g_dlg_bg) {
        if (evt == KEY_EVT_SHORT) {
            if (id == KEY_ID_M) { view_close_dialog(0); return; }
            if (id == KEY_ID_UP || id == KEY_ID_DOWN) {
                g_dlg_focus_yes = !g_dlg_focus_yes;
                lv_group_focus_obj(g_dlg_focus_yes ? g_dlg_btn_yes : g_dlg_btn_no);
                return;
            }
            if (id == KEY_ID_OK) { view_close_dialog(g_dlg_focus_yes); return; }
        }
        return;
    }

    /* AI 弹窗激活时拦截所有按键 */
    if (g_ai_dlg_bg) {
        if (evt == KEY_EVT_SHORT) {
            if (id == KEY_ID_M) { view_close_ai_dialog(-1); return; }
            if (id == KEY_ID_UP || id == KEY_ID_DOWN) {
                g_ai_focus = !g_ai_focus;
                view_ai_dlg_update_highlight();
                lv_group_focus_obj(g_ai_focus ? g_ai_btn_object : g_ai_btn_text);
                return;
            }
            if (id == KEY_ID_OK) { view_close_ai_dialog(g_ai_focus); return; }
        }
        return;
    }

    /* 长按 OK → 弹出删除确认（非播放状态） */
    if (evt == KEY_EVT_LONG_START && id == KEY_ID_OK && !g_playing) {
        view_show_delete_dialog();
        return;
    }

    /* 长按 AI → 弹出 AI 识别选项（非视频相册，非播放状态） */
    if (evt == KEY_EVT_LONG_START && id == KEY_ID_AI && !g_playing) {
        if (!os_strstr(g_folder, "video"))
            view_show_ai_dialog();
        return;
    }

    if (evt != KEY_EVT_SHORT) return;

    switch (id) {
    case KEY_ID_UP:
        if (g_file_index > 0) {
            g_file_index--;
            show_current_file();
        }
        break;

    case KEY_ID_DOWN:
        if (g_file_index < g_total_files - 1) {
            g_file_index++;
            show_current_file();
        }
        break;

    case KEY_ID_OK:
        if (g_playing) {
            g_paused = !g_paused;
        }
        break;

    case KEY_ID_M:
        ui_manager_pop();
        break;

    default:
        break;
    }
}

/* ========== 公共接口 ========== */

void ui_album_view_set_file(const char *folder_path, const char *filename,
                            uint16_t file_index, uint16_t total_files)
{
    if (folder_path) {
        os_strncpy(g_folder, folder_path, sizeof(g_folder) - 1);
        g_folder[sizeof(g_folder) - 1] = '\0';
    }
    g_file_index = file_index;
    (void)filename;
    (void)total_files;
}

static ui_page_t album_view_page = {
    PAGE_ALBUM_VIEW,
    view_create,
    view_destroy,
    view_on_key,
    NULL
};

void ui_album_view_register(void)
{
    ui_page_register(&album_view_page);
}

#else /* FS_EN == 0 */

static lv_obj_t *view_create(ui_page_t *page, lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
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

static void view_destroy(ui_page_t *page)
{
    if (page->root_obj) { lv_obj_del(page->root_obj); page->root_obj = NULL; }
}

static void view_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    if (evt == KEY_EVT_SHORT && (id == KEY_ID_M || id == KEY_ID_OK))
        ui_manager_pop();
}

static ui_page_t album_view_page = {
    PAGE_ALBUM_VIEW, view_create, view_destroy, view_on_key, NULL
};

void ui_album_view_register(void) { ui_page_register(&album_view_page); }
void ui_album_view_set_file(const char *fp, const char *fn,
                            uint16_t fi, uint16_t tf) { (void)fp;(void)fn;(void)fi;(void)tf; }

#endif /* FS_EN */
