/* 照相机页面 — DVP 全屏预览 + 拍照保存到 SD 卡
 *
 * 参考: sdk/app/test_demo/AT_save_photo.c
 *
 * 数据流:
 *   DVP摄像头 → VPP(YUV) → preview_encode_vpp(缩放240x320) → R_VIDEO_P0 → LCD
 *                       ↓
 *                  S_JPEG编码 → R_LVGL_TAKEPHOTO → recv_real_data → SD卡/回调
 *
 * 页面无可见 LVGL 控件，全部透明让 Video P0 图层显示摄像头画面。
 * OK键拍照（闪屏+快门音），M键返回。
 *
 * 流管理和拍照功能通过 ui_camera.h 导出，供 OCR 等页面复用。
 */
#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "lvgl/lvgl.h"
#include "stream_frame.h"
#include "lvgl_ui.h"
#include "custom_mem/custom_mem.h"
#include "fatfs/osal_file.h"
#include "video_app/video_app.h"
#include "ui_manager.h"
#include "ui_camera.h"
#include "app_key.h"

#if DVP_EN

/* ========== 常量 ========== */

#define PREVIEW_W   240
#define PREVIEW_H   320

/* ========== 模块状态 ========== */

static stream *g_preview_s   = NULL;
static stream *g_jpg_s       = NULL;
static stream *g_takephoto_s = NULL;
static lv_timer_t *g_photo_timer = NULL;
static volatile uint8_t g_capturing = 0;

/* 拍照回调 — 设置后拍照数据传给回调，不存SD卡 */
static camera_photo_cb_t g_photo_cb = NULL;

/* 防花屏遮罩 */
static lv_obj_t *g_splash_cover = NULL;
static lv_timer_t *g_splash_timer = NULL;

/* 闪屏特效 */
static lv_obj_t *g_flash_overlay = NULL;
static lv_timer_t *g_flash_timer = NULL;

/* 外部函数（预编译库 sd_save.c） */
extern int no_frame_record_video2(void *fp, void *d, int flen);

/* 快门音效 */
#include "play_pcmtone.h"

/* ========== 回调设置 ========== */

void camera_set_photo_cb(camera_photo_cb_t cb) { g_photo_cb = cb; }
int  camera_is_capturing(void) { return g_capturing; }

/* ========== 流回调 ========== */

static int camera_opcode_func(stream *s, void *priv, int opcode)
{
    /* 不在 STREAM_OPEN_EXIT 时自动 enable。
     * 原因：JPEG 编码器启动时第一帧可能 corrupt（"jpg done len err:9"），
     * 若 takephoto 流立即接收此帧会导致 lvd fault 死机。
     * 改为 camera_take_photo() 中按需 enable，此时编码器已稳定。
     */
    (void)s;
    (void)priv;
    (void)opcode;
    return 0;
}

/* ========== 防花屏遮罩 ========== */

static void splash_timer_cb(lv_timer_t *t)
{
    if (g_splash_cover) {
        lv_obj_del(g_splash_cover);
        g_splash_cover = NULL;
        os_printf("[camera] splash cover removed\r\n");
    }
    g_splash_timer = NULL;
}

void camera_create_splash(lv_obj_t *parent)
{
    g_splash_cover = lv_obj_create(parent);
    lv_obj_remove_style_all(g_splash_cover);
    lv_obj_set_size(g_splash_cover, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_splash_cover, 0, 0);
    lv_obj_set_style_bg_color(g_splash_cover, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(g_splash_cover, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_splash_cover, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    g_splash_timer = lv_timer_create(splash_timer_cb, 300, NULL);
    lv_timer_set_repeat_count(g_splash_timer, 1);
}

void camera_cleanup_splash(void)
{
    if (g_splash_timer) {
        lv_timer_del(g_splash_timer);
        g_splash_timer = NULL;
    }
    g_splash_cover = NULL;
}

/* ========== 流管理 ========== */

void camera_start_streams(void)
{
    os_printf("[camera] creating preview stream (640x480 -> %dx%d)\r\n", PREVIEW_W, PREVIEW_H);

    /* 只启动预览流。JPEG 流延后到拍照时创建，
     * 避免 JPEG 编码器启动第一帧 corrupt 导致 lvd fault 死机。 */
    g_jpg_s = NULL;

    g_preview_s = scale3_stream_not_bind(S_PREVIEW_ENCODE_VPP, 640, 480,
                                          PREVIEW_W, PREVIEW_H, YUV_P0);
    if (g_preview_s) {
        streamSrc_bind_streamDest(g_preview_s, R_VIDEO_P0);
        enable_stream(g_preview_s, 1);
        os_printf("[camera] preview stream started\r\n");
    } else {
        os_printf("[camera] preview stream FAILED\r\n");
    }

    g_takephoto_s = open_stream_available(R_LVGL_TAKEPHOTO, 0, 1,
                                           camera_opcode_func, NULL);
    g_capturing = 0;
    g_photo_timer = NULL;
}

void camera_stop_streams(void)
{
    if (g_takephoto_s) {
        close_stream(g_takephoto_s);
        g_takephoto_s = NULL;
    }
    if (g_jpg_s) {
        close_stream(g_jpg_s);
        g_jpg_s = NULL;
    }
    if (g_preview_s) {
        enable_stream(g_preview_s, 0);
        close_stream(g_preview_s);
        g_preview_s = NULL;
    }
}

void camera_stop_preview(void)
{
    if (g_preview_s) {
        enable_stream(g_preview_s, 0);
        close_stream(g_preview_s);
        g_preview_s = NULL;
        os_printf("[camera] preview stopped (Video P0 removed)\r\n");
    }
}

/* ========== 拍照 ========== */

static void photo_timer_cb(lv_timer_t *t)
{
    if (!g_takephoto_s) return;

    struct data_structure *data_s = recv_real_data(g_takephoto_s);
    if (!data_s) return;

    g_capturing = 0;
    enable_stream(g_takephoto_s, 0);

    uint32_t flen = get_stream_real_data_len(data_s);

    if (g_photo_cb) {
        /* 回调模式 — 先存临时文件，再回调。
         * no_frame_record_video2 知道 data_structure 内部结构，
         * 直接用 data_s->data 可能不是有效 JPEG 数据。 */
        void *fp = osal_fopen("0:/ocr_tmp.jpg", "wb+");
        if (fp) {
            no_frame_record_video2(fp, data_s, flen);
            osal_fclose(fp);
            os_printf("[camera] OCR temp file saved (%u bytes)\r\n", flen);
        }
        g_photo_cb((const uint8_t *)data_s->data, flen);
    } else {
        /* 默认模式 — 保存到 SD 卡 */
        char filename[64];
        os_sprintf(filename, "0:/photo/IMG_%04d.jpg", (uint32_t)(os_jiffies() % 9999));
        os_printf("[camera] saving: %s (%u bytes)\r\n", filename, flen);

        void *fp = osal_fopen(filename, "wb+");
        if (fp) {
            no_frame_record_video2(fp, data_s, flen);
            osal_fclose(fp);
            os_printf("[camera] photo saved: %s\r\n", filename);
        } else {
            os_printf("[camera] save failed: %s\r\n", filename);
        }
    }

    free_data(data_s);
    while ((data_s = recv_real_data(g_takephoto_s)) != NULL)
        free_data(data_s);

    /* 关闭 JPEG 流（停止编码器硬件） */
    if (g_jpg_s) {
        close_stream(g_jpg_s);
        g_jpg_s = NULL;
    }

    if (g_photo_timer) {
        lv_timer_del(g_photo_timer);
        g_photo_timer = NULL;
    }
}

void camera_take_photo(void)
{
    g_capturing = 1;
    play_pcmtone((pcmtone_struct *)&shottone);

    /* 拍照时才创建 JPEG 流（启动编码器硬件）。
     * 不要用 start_jpeg()，它创建的是另一个 S_JPEG 实例，
     * 与 g_jpg_s 冲突会导致 BUF len err 崩溃。
     */
    if (!g_jpg_s) {
        g_jpg_s = new_video_app_stream_with_mode(S_JPEG, 1, 0, 0);
        if (g_jpg_s) {
            streamSrc_bind_streamDest(g_jpg_s, R_LVGL_TAKEPHOTO);
            enable_stream(g_jpg_s, 1);
            os_printf("[camera] JPEG stream created for capture\r\n");
        }
    }

    /* drain 掉编码器启动的 corrupt 第一帧，确保拿到有效帧 */
    if (g_takephoto_s) {
        struct data_structure *old;
        enable_stream(g_takephoto_s, 0);
        while ((old = recv_real_data(g_takephoto_s)) != NULL) {
            free_data(old);
        }
        enable_stream(g_takephoto_s, 1);
    }

    if (g_takephoto_s) {
        g_photo_timer = lv_timer_create(photo_timer_cb, 10, NULL);
        lv_timer_set_repeat_count(g_photo_timer, 500);
    }
}

void camera_cleanup_photo(void)
{
    if (g_photo_timer) {
        lv_timer_del(g_photo_timer);
        g_photo_timer = NULL;
    }
    if (g_capturing) {
        if (g_jpg_s) {
            close_stream(g_jpg_s);
            g_jpg_s = NULL;
        }
        g_capturing = 0;
    }
}

/* ========== 闪屏特效 ========== */

static void flash_timer_cb(lv_timer_t *t)
{
    if (g_flash_overlay) {
        lv_obj_del(g_flash_overlay);
        g_flash_overlay = NULL;
    }
    if (g_flash_timer) {
        lv_timer_del(g_flash_timer);
        g_flash_timer = NULL;
    }
}

void camera_flash_effect(lv_obj_t *parent)
{
    if (!parent) return;

    g_flash_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(g_flash_overlay);
    lv_obj_set_size(g_flash_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_flash_overlay, 0, 0);
    lv_obj_set_style_bg_color(g_flash_overlay, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_flash_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_flash_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    g_flash_timer = lv_timer_create(flash_timer_cb, 150, NULL);
    lv_timer_set_repeat_count(g_flash_timer, 1);
}

/* ========== SD 卡目录初始化 ========== */

static void ensure_photo_dir(void)
{
    FRESULT res = f_mkdir("0:/photo");
    if (res == FR_OK) {
        os_printf("[camera] Created photo directory\r\n");
    }
}

/* ========== 页面回调 ========== */

static lv_obj_t *camera_create(ui_page_t *page, lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    camera_create_splash(root);

    /* Dummy focusable object for key input */
    lv_obj_t *dummy = lv_obj_create(root);
    lv_obj_remove_style_all(dummy);
    lv_obj_set_size(dummy, 1, 1);
    lv_obj_set_pos(dummy, 0, 0);
    lv_obj_add_flag(dummy, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_add_obj(ui_manager_group(), dummy);
    lv_group_focus_obj(dummy);

    /* 确保 DCIM 目录存在 */
    ensure_photo_dir();

    /* 默认 SD 卡模式 */
    camera_set_photo_cb(NULL);
    camera_start_streams();

    os_printf("[camera] page created\r\n");
    return root;
}

static void camera_destroy(ui_page_t *page)
{
    camera_cleanup_photo();
    camera_cleanup_splash();

    /* 清理闪屏特效 */
    if (g_flash_timer) {
        lv_timer_del(g_flash_timer);
        g_flash_timer = NULL;
    }
    g_flash_overlay = NULL;

    camera_stop_streams();

    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }

    os_printf("[camera] page destroyed\r\n");
}

static void camera_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    if (evt != KEY_EVT_SHORT) return;

    if (id == KEY_ID_OK && !g_capturing) {
        camera_flash_effect(page->root_obj);
        camera_take_photo();
        os_printf("[camera] take photo triggered\r\n");
        return;
    }

    if (id == KEY_ID_M) {
        ui_manager_pop();
        return;
    }
}

/* ========== 页面实例 ========== */

static ui_page_t camera_page = {
    PAGE_CAMERA,
    camera_create,
    camera_destroy,
    camera_on_key,
    NULL
};

void ui_camera_register(void)
{
    ui_page_register(&camera_page);
}

#else /* DVP_EN == 0: 编译占位 */

static lv_obj_t *camera_create(ui_page_t *page, lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    lv_obj_t *lbl = lv_label_create(root);
    lv_label_set_text(lbl, "\xe6\x91\x84\xe5\x83\x8f\xe5\xa4\xb4\xe6\x9c\xaa\xe5\x90\xaf\xe7\x94\xa8"); /* 摄像头未启用 */
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

static void camera_destroy(ui_page_t *page)
{
    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
}

static void camera_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    if (evt == KEY_EVT_SHORT && (id == KEY_ID_M || id == KEY_ID_OK))
        ui_manager_pop();
}

static ui_page_t camera_page = {
    PAGE_CAMERA,
    camera_create,
    camera_destroy,
    camera_on_key,
    NULL
};

void ui_camera_register(void)
{
    ui_page_register(&camera_page);
}

/* 非 DVP 模式下提供空实现 */
void camera_set_photo_cb(camera_photo_cb_t cb) { (void)cb; }
void camera_start_streams(void) {}
void camera_stop_streams(void) {}
void camera_take_photo(void) {}
int  camera_is_capturing(void) { return 0; }
void camera_cleanup_photo(void) {}
void camera_create_splash(lv_obj_t *parent) { (void)parent; }
void camera_cleanup_splash(void) {}
void camera_flash_effect(lv_obj_t *parent) { (void)parent; }
void camera_stop_preview(void) {}

#endif /* DVP_EN */
