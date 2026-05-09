/* 拍图识字 / 拍图识物 页面 — 复用 camera 模块的预览/拍照，拍照后走识别流程
 *
 * 支持两种模式 (g_ocr_mode):
 *   OCR_MODE_TEXT     — 拍图识字，使用 object_ocr_prompt，发送 "请识别图片中的文字"
 *   OCR_MODE_OBJECT   — 拍图识物，使用 object_vision_prompt，发送 "图片中是什么？"
 *
 *
 * 数据流（预览/拍照复用 ui_camera.c）:
 *   DVP摄像头 → VPP(YUV) → preview_encode_vpp → R_VIDEO_P0 → LCD
 *                       ↓
 *                  S_JPEG → R_LVGL_TAKEPHOTO → camera 模块 → ocr_photo_cb
 *
 * 状态机:
 *   OCR_PREVIEW    — 摄像头实时预览，OK拍照
 *   OCR_CONFIRM    — 显示照片 + 确认弹窗（OK确认 / 重拍）
 *   OCR_RECOGNIZING— 聊天布局：右上缩略图 + 左侧实时更新文字
 *   OCR_RESULT     — 聊天布局：完整结果，UP/DOWN滚动
 */
#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "lvgl/lvgl.h"
#include "custom_mem/custom_mem.h"
#include "ui_manager.h"
#include "ui_ocr.h"
#include "ui_camera.h"
#include "ui_album_decode.h"
#include "fatfs/osal_file.h"
#include "app_key.h"

/* 简体中文子集字体（GB2312 Level 1 前 2368 字 + 标点） */
extern lv_font_t lv_font_cn_20_4bpp;
extern lv_font_t lv_font_cn_16_4bpp;
#define OCR_TEXT_FONT &lv_font_cn_20_4bpp
#define OCR_HINT_FONT &lv_font_cn_16_4bpp

#if DVP_EN

/* ========== 布局常量 ========== */

#define OCR_IMG_W       240
#define OCR_IMG_H       320

#define STATUS_BAR_H    26
#define BOTTOM_HINT_H   26

/* 缩略图尺寸 (3:4 比例) */
#define THUMB_W         72
#define THUMB_H         96
#define THUMB_MARGIN_X  6
#define THUMB_MARGIN_Y  6

/* 聊天气泡 */
#define BUBBLE_MARGIN   8
#define BUBBLE_W        224
#define BUBBLE_TOP_Y    (STATUS_BAR_H + THUMB_H + THUMB_MARGIN_Y * 2)
#define BUBBLE_H        (320 - BOTTOM_HINT_H - BUBBLE_TOP_Y - 4)

/* 颜色 */
#define COLOR_BG_BLUE     lv_color_hex(0x1890FF)
#define COLOR_FONT_BLACK  lv_color_hex(0x000000)
#define COLOR_FONT_WHITE  lv_color_hex(0xFFFFFF)
#define COLOR_TEXT_DARK   lv_color_hex(0x333333)
#define COLOR_CHAT_BG     lv_color_hex(0xF5F5F5)
#define COLOR_BUBBLE_BG   lv_color_hex(0xFFFFFF)

/* ========== 状态 ========== */

enum {
    OCR_PREVIEW = 0,
    OCR_CONFIRM,
    OCR_RECOGNIZING,
    OCR_RESULT,
};

static uint8_t g_ocr_state = OCR_PREVIEW;

/* JPEG 缓冲（PSRAM） */
static uint8_t *g_ocr_jpeg_buf = NULL;
static uint32_t g_ocr_jpeg_len = 0;

/* RGB 显示缓冲（PSRAM） */
static uint8_t *g_ocr_rgb_buf = NULL;
static lv_img_dsc_t g_ocr_img_dsc;

/* 缩略图缓冲（PSRAM） */
static uint8_t *g_ocr_thumb_buf = NULL;
static lv_img_dsc_t g_ocr_thumb_dsc;

/* ========== LVGL 控件 ========== */

/* 预览+确认阶段 */
static lv_obj_t *g_ocr_img = NULL;
static lv_obj_t *g_confirm_overlay = NULL;
static lv_obj_t *g_confirm_highlight = NULL;
static lv_obj_t *g_confirm_labels[2] = {NULL};
static int8_t g_confirm_idx = 0;

/* 聊天阶段（识别中 + 结果） */
static lv_obj_t *g_chat_root = NULL;      /* 聊天容器，初始隐藏 */
static lv_obj_t *g_chat_thumb = NULL;     /* 右上缩略图 */
static lv_obj_t *g_chat_bubble = NULL;    /* 左侧文字气泡容器 */
static lv_obj_t *g_chat_label = NULL;     /* 文字标签 */
static lv_obj_t *g_chat_hint = NULL;      /* 底部提示 */

/* 状态栏 */
static lv_obj_t *g_status_bat = NULL;
static lv_obj_t *g_status_time = NULL;
static lv_obj_t *g_status_wifi = NULL;

/* OCR 结果回调 — 双缓冲 + volatile 同步
 * BRTC 回调线程写 buf_a，LVGL 定时器线程从 buf_a 安全拷贝到 buf_b
 * g_ocr_flag = 1 表示 buf_a 有新数据待 LVGL 侧取走
 */
static char g_ocr_buf_a[1024];       /* BRTC 回调写入 */
static volatile int g_ocr_buf_a_len = 0;
static volatile int g_ocr_flag = 0;  /* 1 = buf_a 有更新 */
static char g_ocr_buf_b[1024];       /* LVGL 侧本地副本 */
static int g_ocr_result_len = 0;
static int g_ocr_last_check_len = 0;
static int g_ocr_stable_count = 0;
static int g_ocr_total_ticks = 0;      /* 总轮询次数 */
static lv_timer_t *g_ocr_result_timer = NULL;

/* 外部 BRTC 函数 */
extern void brtc_send_vision_image(const uint8_t *jpeg_data, size_t len);
extern void brtc_send_vision_object(const uint8_t *jpeg_data, size_t len);
extern void brtc_register_ocr_callback(void (*cb)(const char *text, int len));
extern void brtc_restore_image_mode(void);
extern void brtc_interrupt_tts(void);

/* 识别模式 */
static uint8_t g_ocr_mode = OCR_MODE_TEXT;

/* 文件模式：非空则跳过摄像头，直接读取文件识别 */
static char g_ocr_file_path[196];
static uint8_t g_ocr_file_mode;  /* 1=从文件进入，确认页重拍改为返回 */

void ui_ocr_set_file(const char *filepath, uint8_t mode)
{
    if (filepath) {
        os_strncpy(g_ocr_file_path, filepath, sizeof(g_ocr_file_path) - 1);
        g_ocr_file_path[sizeof(g_ocr_file_path) - 1] = '\0';
    } else {
        g_ocr_file_path[0] = '\0';
    }
    g_ocr_mode = mode;
}

/* 外部状态栏支持 */
#include "../res/res_icons.h"
#include "../app_power.h"
#include "../app_wifi_config.h"

/* ========== RGB565 缩略图降采样 ========== */

static void downsample_rgb565(const uint8_t *src, uint8_t *dst,
                               int sw, int sh, int dw, int dh)
{
    int r, c;
    for (r = 0; r < dh; r++) {
        int sr = r * sh / dh;
        for (c = 0; c < dw; c++) {
            int sc = c * sw / dw;
            ((uint16_t *)dst)[r * dw + c] = ((const uint16_t *)src)[sr * sw + sc];
        }
    }
}

/* ========== 确认弹窗高亮 ========== */

static void ocr_confirm_update_highlight(void)
{
    if (!g_confirm_highlight) return;
    lv_obj_set_pos(g_confirm_highlight, g_confirm_idx == 0 ? 25 : 105, 42);
    for (int i = 0; i < 2; i++) {
        if (g_confirm_labels[i]) {
            lv_obj_set_style_text_color(g_confirm_labels[i],
                i == g_confirm_idx ? COLOR_FONT_WHITE : COLOR_FONT_BLACK, 0);
        }
    }
}

/* ========== 延迟解码定时器 ========== */

static void ocr_decode_timer_cb(lv_timer_t *t)
{
    int dec_ok = -1;
    os_printf("[ocr] decode timer, jpeg_len=%d\r\n", g_ocr_jpeg_len);

    camera_stop_streams();

    if (g_ocr_rgb_buf) {
        dec_ok = album_decode_jpeg_file("0:/ocr_tmp.jpg",
                                        g_ocr_rgb_buf, OCR_IMG_W, OCR_IMG_H);
        os_printf("[ocr] decode=%d\r\n", dec_ok);
    }

    if (dec_ok == 0) {
        g_ocr_img_dsc.data = g_ocr_rgb_buf;
        lv_img_set_src(g_ocr_img, &g_ocr_img_dsc);
        lv_obj_clear_flag(g_ocr_img, LV_OBJ_FLAG_HIDDEN);

        /* 同时生成缩略图 */
        if (g_ocr_thumb_buf) {
            downsample_rgb565(g_ocr_rgb_buf, g_ocr_thumb_buf,
                              OCR_IMG_W, OCR_IMG_H, THUMB_W, THUMB_H);
            g_ocr_thumb_dsc.data = g_ocr_thumb_buf;
        }
    }

    /* 显示确认弹窗 */
    if (g_confirm_overlay) lv_obj_clear_flag(g_confirm_overlay, LV_OBJ_FLAG_HIDDEN);
    g_confirm_idx = 0;
    ocr_confirm_update_highlight();
}

/* ========== 拍照回调 ========== */

static void ocr_photo_cb(const uint8_t *jpeg_data, uint32_t jpeg_len)
{
    (void)jpeg_data;
    os_printf("[ocr] captured, reading from temp file\r\n");

    if (g_ocr_jpeg_buf) {
        void *fp = osal_fopen("0:/ocr_tmp.jpg", "rb");
        if (fp) {
            g_ocr_jpeg_len = osal_fread(g_ocr_jpeg_buf, 1, 200 * 1024, fp);
            osal_fclose(fp);
            os_printf("[ocr] read JPEG: %u bytes\r\n", g_ocr_jpeg_len);
        } else {
            g_ocr_jpeg_len = 0;
        }
    }

    if (g_ocr_jpeg_buf && g_ocr_jpeg_len > 0) {
        g_ocr_state = OCR_CONFIRM;
        camera_stop_preview();

        lv_timer_t *decode_timer = lv_timer_create(ocr_decode_timer_cb, 300, NULL);
        lv_timer_set_repeat_count(decode_timer, 1);
    }
}

/* ========== OCR 结果回调 ========== */

static void ocr_result_cb(const char *text, int len)
{
    /* BRTC 发的是累积字幕（含之前全部文字），直接整体替换 */
    if (len > 0 && len < (int)sizeof(g_ocr_buf_a) - 1) {
        memcpy(g_ocr_buf_a, text, len);
        g_ocr_buf_a[len] = '\0';
        g_ocr_buf_a_len = len;
        g_ocr_flag = 1;
    }
    os_printf("[ocr] result chunk: %.*s (total %d)\r\n", len, text, len);
}

/* ========== 轮询 OCR 结果（实时更新 + 稳定检测） ========== */

static void ocr_result_timer_cb(lv_timer_t *t)
{
    g_ocr_total_ticks++;

    /* 从 BRTC 线程缓冲安全拷贝到 LVGL 本地缓冲 */
    if (g_ocr_flag) {
        int len = g_ocr_buf_a_len;
        if (len > 0 && len < (int)sizeof(g_ocr_buf_b) - 1) {
            memcpy(g_ocr_buf_b, (const char *)g_ocr_buf_a, len);
            g_ocr_buf_b[len] = '\0';
            g_ocr_result_len = len;
        }
        g_ocr_flag = 0;
    }

    /* 实时更新文字 */
    if (g_ocr_result_len > 0 && g_chat_label) {
        lv_label_set_text(g_chat_label, g_ocr_buf_b);
    }

    /* 收到文字后才做稳定检测；没收到文字只做最大等待 */
    if (g_ocr_result_len > 0) {
        if (g_ocr_result_len == g_ocr_last_check_len) {
            g_ocr_stable_count++;
        } else {
            g_ocr_stable_count = 0;
            g_ocr_last_check_len = g_ocr_result_len;
        }
    }

    /* 文本稳定 10 * 200ms = 2秒，或最大等待 10秒无任何回复 */
    int done = 0;
    if (g_ocr_result_len > 0 && g_ocr_stable_count >= 10) {
        done = 1;
    } else if (g_ocr_total_ticks >= 50) {  /* 50 * 200ms = 10秒超时 */
        done = 1;
    }

    if (done) {
        if (g_ocr_result_timer) {
            lv_timer_del(g_ocr_result_timer);
            g_ocr_result_timer = NULL;
        }
        g_ocr_state = OCR_RESULT;
        os_printf("[ocr] -> OCR_RESULT, len=%d\r\n", g_ocr_result_len);
    }
}

/* ========== 页面回调 ========== */

static lv_obj_t *ocr_create(ui_page_t *page, lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* 防花屏遮罩 */
    camera_create_splash(root);

    /* === 缓冲区分配 === */
    g_ocr_jpeg_buf = (uint8_t *)custom_malloc_psram(200 * 1024);
    g_ocr_rgb_buf  = (uint8_t *)custom_malloc_psram(OCR_IMG_W * OCR_IMG_H * 2);
    g_ocr_thumb_buf = (uint8_t *)custom_malloc_psram(THUMB_W * THUMB_H * 2);
    if (g_ocr_rgb_buf)  os_memset(g_ocr_rgb_buf, 0, OCR_IMG_W * OCR_IMG_H * 2);
    if (g_ocr_thumb_buf) os_memset(g_ocr_thumb_buf, 0xFF, THUMB_W * THUMB_H * 2);

    /* 全屏图片 dsc */
    g_ocr_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    g_ocr_img_dsc.header.w = OCR_IMG_W;
    g_ocr_img_dsc.header.h = OCR_IMG_H;
    g_ocr_img_dsc.data_size = OCR_IMG_W * OCR_IMG_H * 2;
    g_ocr_img_dsc.data = g_ocr_rgb_buf;

    /* 缩略图 dsc */
    g_ocr_thumb_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    g_ocr_thumb_dsc.header.w = THUMB_W;
    g_ocr_thumb_dsc.header.h = THUMB_H;
    g_ocr_thumb_dsc.data_size = THUMB_W * THUMB_H * 2;
    g_ocr_thumb_dsc.data = g_ocr_thumb_buf;

    /* === 预览+确认阶段控件 === */

    g_ocr_img = lv_img_create(root);
    lv_img_set_src(g_ocr_img, &g_ocr_img_dsc);
    lv_obj_align(g_ocr_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(g_ocr_img, LV_OBJ_FLAG_HIDDEN);

    /* 确认弹窗（无全屏遮罩） */
    g_confirm_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(g_confirm_overlay);
    lv_obj_set_size(g_confirm_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_confirm_overlay, 0, 0);
    lv_obj_set_style_bg_opa(g_confirm_overlay, LV_OPA_TRANSP, 0);
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
    lv_label_set_text(ctext,
        g_ocr_mode == OCR_MODE_OBJECT
            ? "\xe6\x98\xaf\xe5\x90\xa6\xe5\x8f\x91\xe9\x80\x81\xe8\xaf\x86\xe7\x89\xa9?"   /* 是否发送识物? */
            : "\xe6\x98\xaf\xe5\x90\xa6\xe5\x8f\x91\xe9\x80\x81\xe8\xaf\x86\xe5\x88\xab?");  /* 是否发送识别? */
    lv_obj_align(ctext, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_text_color(ctext, COLOR_FONT_BLACK, 0);
    lv_obj_set_style_text_font(ctext, OCR_TEXT_FONT, 0);

    g_confirm_highlight = lv_obj_create(cbox);
    lv_obj_remove_style_all(g_confirm_highlight);
    lv_obj_set_size(g_confirm_highlight, 75, 28);
    lv_obj_set_style_bg_color(g_confirm_highlight, COLOR_BG_BLUE, 0);
    lv_obj_set_style_bg_opa(g_confirm_highlight, LV_OPA_80, 0);
    lv_obj_set_style_radius(g_confirm_highlight, 6, 0);
    lv_obj_clear_flag(g_confirm_highlight, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    g_confirm_labels[0] = lv_label_create(cbox);
    lv_label_set_text(g_confirm_labels[0], "OK");
    lv_obj_set_pos(g_confirm_labels[0], 50, 50);
    lv_obj_set_style_text_color(g_confirm_labels[0], COLOR_FONT_WHITE, 0);
    lv_obj_set_style_text_font(g_confirm_labels[0], OCR_TEXT_FONT, 0);

    g_confirm_labels[1] = lv_label_create(cbox);
    lv_label_set_text(g_confirm_labels[1],
        g_ocr_file_mode ? "\xe9\x87\x8d\xe9\x80\x89" : "\xe9\x87\x8d\xe6\x8b\x8d"); /* 重选/重拍 */
    lv_obj_set_pos(g_confirm_labels[1], 127, 50);
    lv_obj_set_style_text_color(g_confirm_labels[1], COLOR_FONT_BLACK, 0);
    lv_obj_set_style_text_font(g_confirm_labels[1], OCR_TEXT_FONT, 0);

    /* === 聊天阶段控件（初始隐藏） === */

    g_chat_root = lv_obj_create(root);
    lv_obj_remove_style_all(g_chat_root);
    lv_obj_set_size(g_chat_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_chat_root, 0, 0);
    lv_obj_clear_flag(g_chat_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(g_chat_root, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(g_chat_root, LV_OBJ_FLAG_HIDDEN);

    /* Wallpaper background */
    lv_obj_t *chat_bg = lv_img_create(g_chat_root);
    lv_img_set_src(chat_bg, &Wallpaper0);
    lv_obj_set_pos(chat_bg, 0, 0);
    lv_obj_clear_flag(chat_bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* White transparent overlay */
    lv_obj_t *chat_overlay = lv_obj_create(g_chat_root);
    lv_obj_remove_style_all(chat_overlay);
    lv_obj_set_size(chat_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(chat_overlay, 0, 0);
    lv_obj_clear_flag(chat_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(chat_overlay, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(chat_overlay, LV_OPA_30, 0);

    /* 聊天阶段 - 状态栏 */
    lv_obj_t *sbar = lv_obj_create(g_chat_root);
    lv_obj_remove_style_all(sbar);
    lv_obj_set_size(sbar, LV_PCT(100), STATUS_BAR_H);
    lv_obj_set_pos(sbar, 0, 0);
    lv_obj_clear_flag(sbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(sbar, LV_OPA_30, 0);

    g_status_bat = lv_img_create(sbar);
    {
        enum bat_level lvl = app_battery_get_level();
        lv_img_set_src(g_status_bat, &icon_bat2);
        (void)lvl;
    }
    lv_obj_align(g_status_bat, LV_ALIGN_LEFT_MID, 2, 0);

    g_status_time = lv_label_create(sbar);
    lv_label_set_text(g_status_time, "00:00");
    lv_obj_align(g_status_time, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(g_status_time, COLOR_TEXT_DARK, 0);

    g_status_wifi = lv_img_create(sbar);
    lv_img_set_src(g_status_wifi, g_wifi_connected ? &icon_wifi : &icon_no_wifi);
    lv_obj_align(g_status_wifi, LV_ALIGN_RIGHT_MID, -4, 0);

    ui_manager_set_bat_img(g_status_bat);
    ui_manager_set_wifi_img(g_status_wifi);
    ui_manager_set_time_label(g_status_time);

    /* 聊天阶段 - 右上缩略图 */
    g_chat_thumb = lv_img_create(g_chat_root);
    lv_img_set_src(g_chat_thumb, &g_ocr_thumb_dsc);
    lv_obj_set_pos(g_chat_thumb, 240 - THUMB_W - THUMB_MARGIN_X,
                   STATUS_BAR_H + THUMB_MARGIN_Y);
    lv_obj_set_style_radius(g_chat_thumb, 6, 0);
    lv_obj_set_style_clip_corner(g_chat_thumb, 1, 0);
    lv_obj_set_style_border_width(g_chat_thumb, 1, 0);
    lv_obj_set_style_border_color(g_chat_thumb, lv_color_hex(0xDDDDDD), 0);

    /* 聊天阶段 - 左侧文字气泡 */
    g_chat_bubble = lv_obj_create(g_chat_root);
    lv_obj_remove_style_all(g_chat_bubble);
    lv_obj_set_size(g_chat_bubble, BUBBLE_W, BUBBLE_H);
    lv_obj_set_pos(g_chat_bubble, BUBBLE_MARGIN, BUBBLE_TOP_Y);
    lv_obj_set_style_bg_color(g_chat_bubble, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_chat_bubble, LV_OPA_80, 0);
    lv_obj_set_style_radius(g_chat_bubble, 12, 0);
    lv_obj_set_style_pad_all(g_chat_bubble, 8, 0);
    lv_obj_add_flag(g_chat_bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g_chat_bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(g_chat_bubble, 2, 0);
    lv_obj_set_style_border_color(g_chat_bubble, COLOR_BG_BLUE, 0);
    lv_obj_set_style_border_opa(g_chat_bubble, LV_OPA_60, 0);
    lv_obj_set_style_shadow_width(g_chat_bubble, 10, 0);
    lv_obj_set_style_shadow_spread(g_chat_bubble, 2, 0);
    lv_obj_set_style_shadow_color(g_chat_bubble, COLOR_BG_BLUE, 0);
    lv_obj_set_style_shadow_opa(g_chat_bubble, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_x(g_chat_bubble, 0, 0);
    lv_obj_set_style_shadow_ofs_y(g_chat_bubble, 0, 0);

    g_chat_label = lv_label_create(g_chat_bubble);
    lv_label_set_text(g_chat_label,
                      "\xe8\xaf\x86\xe5\x88\xab\xe4\xb8\xad..."); /* 识别中... */
    lv_obj_set_style_text_color(g_chat_label, COLOR_TEXT_DARK, 0);
    lv_obj_set_style_text_font(g_chat_label, OCR_TEXT_FONT, 0);
    lv_obj_set_width(g_chat_label, BUBBLE_W - 16); /* 减去 padding */
    lv_label_set_long_mode(g_chat_label, LV_LABEL_LONG_WRAP);

    /* 聊天阶段 - 底部提示 */
    g_chat_hint = lv_label_create(g_chat_root);
    lv_label_set_text(g_chat_hint, "M\xe8\xbf\x94\xe5\x9b\x9e");
    lv_obj_align(g_chat_hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_text_color(g_chat_hint, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(g_chat_hint, OCR_HINT_FONT, 0);

    /* === 虚拟焦点 === */
    lv_obj_t *dummy = lv_obj_create(root);
    lv_obj_remove_style_all(dummy);
    lv_obj_set_size(dummy, 1, 1);
    lv_obj_set_pos(dummy, 0, 0);
    lv_obj_add_flag(dummy, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_add_obj(ui_manager_group(), dummy);
    lv_group_focus_obj(dummy);

    /* === 启动 === */
    g_ocr_flag = 0;
    g_ocr_buf_a_len = 0;
    g_ocr_buf_a[0] = '\0';
    g_ocr_result_len = 0;
    g_ocr_buf_b[0] = '\0';
    brtc_register_ocr_callback(ocr_result_cb);

    if (g_ocr_file_path[0] != '\0') {
        /* === 文件模式：直接读文件，跳过摄像头 === */
        g_ocr_file_mode = 1;
        g_ocr_state = OCR_CONFIRM;
        g_ocr_jpeg_len = 0;

        /* 读取 JPEG 文件到缓冲区 */
        if (g_ocr_jpeg_buf) {
            void *fp = osal_fopen(g_ocr_file_path, "rb");
            if (fp) {
                g_ocr_jpeg_len = osal_fread(g_ocr_jpeg_buf, 1, 200 * 1024, fp);
                osal_fclose(fp);
                os_printf("[ocr] file mode: read %u bytes from %s\r\n",
                          g_ocr_jpeg_len, g_ocr_file_path);
            } else {
                os_printf("[ocr] file mode: failed to open %s\r\n", g_ocr_file_path);
            }
        }

        /* 解码显示图片 + 缩略图 */
        if (g_ocr_jpeg_len > 0 && g_ocr_rgb_buf) {
            int dec_ok = img_decode_jpeg_mem(g_ocr_jpeg_buf, g_ocr_jpeg_len,
                                              g_ocr_rgb_buf, OCR_IMG_W, OCR_IMG_H);
            if (dec_ok == 0) {
                g_ocr_img_dsc.data = g_ocr_rgb_buf;
                lv_img_set_src(g_ocr_img, &g_ocr_img_dsc);
                lv_obj_clear_flag(g_ocr_img, LV_OBJ_FLAG_HIDDEN);

                if (g_ocr_thumb_buf) {
                    downsample_rgb565(g_ocr_rgb_buf, g_ocr_thumb_buf,
                                      OCR_IMG_W, OCR_IMG_H, THUMB_W, THUMB_H);
                    g_ocr_thumb_dsc.data = g_ocr_thumb_buf;
                }
            }
        }

        /* 显示确认弹窗 */
        if (g_confirm_overlay) lv_obj_clear_flag(g_confirm_overlay, LV_OBJ_FLAG_HIDDEN);
        g_confirm_idx = 0;
        ocr_confirm_update_highlight();

        g_ocr_file_path[0] = '\0';  /* 清除，仅用一次 */
        os_printf("[ocr] page created (file mode)\r\n");
    } else {
        /* === 摄像头模式（原有逻辑）=== */
        g_ocr_file_mode = 0;
        g_ocr_state = OCR_PREVIEW;
        g_ocr_jpeg_len = 0;
        camera_set_photo_cb(ocr_photo_cb);

        {
            extern struct sys_sramheap sram_heap;
            uint32 free_sram = sysheap_freesize(&sram_heap);
            os_printf("[ocr] free SRAM=%u before camera start\r\n", free_sram);
            if (free_sram < 20 * 1024) {
                os_sleep_ms(150);
                free_sram = sysheap_freesize(&sram_heap);
                os_printf("[ocr] after delay, free SRAM=%u\r\n", free_sram);
            }
        }
        camera_start_streams();
        os_printf("[ocr] page created (camera mode)\r\n");
    }
    return root;
}

static void ocr_destroy(ui_page_t *page)
{
    brtc_register_ocr_callback(NULL);
    brtc_restore_image_mode();
    camera_set_photo_cb(NULL);

    if (g_ocr_result_timer) {
        lv_timer_del(g_ocr_result_timer);
        g_ocr_result_timer = NULL;
    }

    camera_cleanup_photo();
    camera_cleanup_splash();
    camera_stop_streams();

    if (g_ocr_jpeg_buf) {
        custom_free_psram(g_ocr_jpeg_buf);
        g_ocr_jpeg_buf = NULL;
    }
    g_ocr_jpeg_len = 0;

    if (g_ocr_rgb_buf) {
        custom_free_psram(g_ocr_rgb_buf);
        g_ocr_rgb_buf = NULL;
    }

    if (g_ocr_thumb_buf) {
        custom_free_psram(g_ocr_thumb_buf);
        g_ocr_thumb_buf = NULL;
    }

    /* 清空指针 */
    g_ocr_img = NULL;
    g_confirm_overlay = NULL;
    g_confirm_highlight = NULL;
    for (int i = 0; i < 2; i++) g_confirm_labels[i] = NULL;
    g_chat_root = NULL;
    g_chat_thumb = NULL;
    g_chat_bubble = NULL;
    g_chat_label = NULL;
    g_chat_hint = NULL;
    g_status_bat = NULL;
    g_status_time = NULL;
    g_status_wifi = NULL;

    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
    os_printf("[ocr] page destroyed\r\n");
}

static void ocr_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    switch (g_ocr_state) {

    case OCR_PREVIEW:
        if (evt != KEY_EVT_SHORT) return;
        if (id == KEY_ID_OK && !camera_is_capturing()) {
            camera_take_photo();
        } else if (id == KEY_ID_M) {
            ui_manager_pop();
        }
        break;

    case OCR_CONFIRM:
        if (evt != KEY_EVT_SHORT) return;
        if (id == KEY_ID_UP) {
            g_confirm_idx = 0;
            ocr_confirm_update_highlight();
        } else if (id == KEY_ID_DOWN) {
            g_confirm_idx = 1;
            ocr_confirm_update_highlight();
        } else if (id == KEY_ID_OK) {
            if (g_confirm_idx == 0) {
                /* OK → 发送识别，切换到聊天布局 */
                if (g_confirm_overlay)
                    lv_obj_add_flag(g_confirm_overlay, LV_OBJ_FLAG_HIDDEN);
                if (g_ocr_img)
                    lv_obj_add_flag(g_ocr_img, LV_OBJ_FLAG_HIDDEN);

                /* 显示聊天界面 */
                if (g_chat_root)
                    lv_obj_clear_flag(g_chat_root, LV_OBJ_FLAG_HIDDEN);

                /* 重置文字为识别中 */
                if (g_chat_label)
                    lv_label_set_text(g_chat_label,
                                      "\xe8\xaf\x86\xe5\x88\xab\xe4\xb8\xad...");

                g_ocr_state = OCR_RECOGNIZING;
                g_ocr_flag = 0;
                g_ocr_buf_a_len = 0;
                g_ocr_buf_a[0] = '\0';
                g_ocr_result_len = 0;
                g_ocr_buf_b[0] = '\0';
                g_ocr_last_check_len = 0;
                g_ocr_stable_count = 0;
                g_ocr_total_ticks = 0;

                if (g_ocr_mode == OCR_MODE_OBJECT)
                    brtc_send_vision_object(g_ocr_jpeg_buf, g_ocr_jpeg_len);
                else
                    brtc_send_vision_image(g_ocr_jpeg_buf, g_ocr_jpeg_len);

                g_ocr_result_timer = lv_timer_create(ocr_result_timer_cb, 200, NULL);
                lv_timer_set_repeat_count(g_ocr_result_timer, -1);
                os_printf("[ocr] sending to vision API\r\n");
            } else {
                /* 重拍 / 返回 */
                if (g_ocr_file_mode) {
                    ui_manager_pop();
                } else {
                    if (g_confirm_overlay)
                        lv_obj_add_flag(g_confirm_overlay, LV_OBJ_FLAG_HIDDEN);
                    if (g_ocr_img)
                        lv_obj_add_flag(g_ocr_img, LV_OBJ_FLAG_HIDDEN);
                    g_ocr_state = OCR_PREVIEW;
                    g_ocr_jpeg_len = 0;
                    camera_start_streams();
                }
            }
        } else if (id == KEY_ID_M) {
            ui_manager_pop();
        }
        break;

    case OCR_RECOGNIZING:
    case OCR_RESULT:
        if (evt != KEY_EVT_SHORT) return;
        if (id == KEY_ID_M) {
            brtc_interrupt_tts();
            os_sleep_ms(200);
            if (g_ocr_result_timer) {
                lv_timer_del(g_ocr_result_timer);
                g_ocr_result_timer = NULL;
            }
            if (g_ocr_file_mode) {
                ui_manager_pop();
            } else {
                if (g_chat_root)
                    lv_obj_add_flag(g_chat_root, LV_OBJ_FLAG_HIDDEN);
                g_ocr_state = OCR_PREVIEW;
                g_ocr_jpeg_len = 0;
                camera_start_streams();
                camera_create_splash(page->root_obj);
            }
        } else if (id == KEY_ID_DOWN && g_chat_bubble) {
            lv_obj_scroll_by(g_chat_bubble, 0, -20, LV_ANIM_OFF);
        } else if (id == KEY_ID_UP && g_chat_bubble) {
            lv_obj_scroll_by(g_chat_bubble, 0, 20, LV_ANIM_OFF);
        }
        break;
    }
}

/* ========== 页面实例 ========== */

static lv_obj_t *ocr_create_text(ui_page_t *page, lv_obj_t *parent)
{
    g_ocr_mode = OCR_MODE_TEXT;
    return ocr_create(page, parent);
}

static lv_obj_t *ocr_create_object(ui_page_t *page, lv_obj_t *parent)
{
    g_ocr_mode = OCR_MODE_OBJECT;
    return ocr_create(page, parent);
}

static ui_page_t ocr_page = {
    PAGE_PHOTO_TEXT,
    ocr_create_text,
    ocr_destroy,
    ocr_on_key,
    NULL
};

static ui_page_t recognize_page = {
    PAGE_RECOGNIZE,
    ocr_create_object,
    ocr_destroy,
    ocr_on_key,
    NULL
};

void ui_ocr_register(void)
{
    ui_page_register(&ocr_page);
    ui_page_register(&recognize_page);
}

#else /* DVP_EN == 0 */

static lv_obj_t *ocr_create_text(ui_page_t *page, lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    lv_obj_t *lbl = lv_label_create(root);
    lv_label_set_text(lbl, "\xe6\x91\x84\xe5\x83\x8f\xe5\xa4\xb4\xe6\x9c\xaa\xe5\x90\xaf\xe7\x94\xa8");
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(lbl, OCR_TEXT_FONT, 0);

    lv_obj_t *dummy = lv_obj_create(root);
    lv_obj_remove_style_all(dummy);
    lv_obj_set_size(dummy, 1, 1);
    lv_obj_set_pos(dummy, 0, 0);
    lv_obj_add_flag(dummy, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_add_obj(ui_manager_group(), dummy);
    lv_group_focus_obj(dummy);

    return root;
}

static lv_obj_t *ocr_create_object(ui_page_t *page, lv_obj_t *parent)
{
    return ocr_create_text(page, parent);
}

static void ocr_destroy(ui_page_t *page)
{
    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
}

static void ocr_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    if (evt == KEY_EVT_SHORT && (id == KEY_ID_M || id == KEY_ID_OK))
        ui_manager_pop();
}

static ui_page_t ocr_page = {
    PAGE_PHOTO_TEXT,
    ocr_create_text,
    ocr_destroy,
    ocr_on_key,
    NULL
};

static ui_page_t recognize_page = {
    PAGE_RECOGNIZE,
    ocr_create_object,
    ocr_destroy,
    ocr_on_key,
    NULL
};

void ui_ocr_register(void)
{
    ui_page_register(&ocr_page);
    ui_page_register(&recognize_page);
}

void ui_ocr_set_file(const char *filepath, uint8_t mode) { (void)filepath; (void)mode; }

#endif /* DVP_EN */
