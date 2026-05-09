/* 图像 JPEG 解码模块
 *
 * JPEG 数据 → 硬件解码+缩放 → YUV420 → 软件 RGB565 转换
 * 供 ui_album_grid.c (缩略图)、ui_album_view.c (大图)、ui_placeholder.c (生图) 共用。
 *
 * 两个入口：
 *   img_decode_jpeg_mem()  — 从内存缓冲区解码（生图场景，数据已在 PSRAM）
 *   album_decode_jpeg_file() — 从 SD 卡文件解码（相册场景，先读到 PSRAM 再调用 mem 版）
 */
#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "dev.h"
#include "devid.h"
#include "hal/scale.h"
#include "hal/jpeg.h"
#include "dev/jpg/hgjpg.h"
#include "lib/lcd/lcd.h"
#include "custom_mem/custom_mem.h"

#include "ui_album_decode.h"

/* ========== JPEG 尺寸解析（支持 SOF0/SOF1/SOF2）========== */

static void jpg_analyze_ex(const uint8_t *buf, uint32_t *size)
{
    uint32_t itk;
    for (itk = 0; itk < 4096; itk++) {
        if (buf[itk] == 0xFF) {
            uint8_t marker = buf[itk + 1];
            if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
                uint8_t *p = (uint8_t *)size;
                p[0] = buf[itk + 6]; p[1] = buf[itk + 5]; p[2] = 0; p[3] = 0;
                p[4] = buf[itk + 8]; p[5] = buf[itk + 7]; p[6] = 0; p[7] = 0;
                return;
            }
            if (marker >= 0xD0 && marker <= 0xD9) continue;
            if (marker == 0x00 || marker == 0xFF) continue;
            if (itk + 3 < 4096) {
                uint16_t seg_len = (buf[itk + 2] << 8) | buf[itk + 3];
                itk += seg_len + 1;
            }
        }
    }
}

/* ========== YUV420 → RGB565 转换 ========== */

static inline int16_t clamp8(int16_t v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void yuv420_to_rgb565(const uint8_t *y_p, const uint8_t *u_p,
                              const uint8_t *v_p, uint16_t *rgb,
                              uint32_t out_w, uint32_t out_h,
                              uint32_t y_stride)
{
    uint32_t uv_stride = y_stride / 2;
    uint32_t row, col;

    for (row = 0; row < out_h; row++) {
        const uint8_t *yr = y_p + row * y_stride;
        const uint8_t *ur = u_p + (row / 2) * uv_stride;
        const uint8_t *vr = v_p + (row / 2) * uv_stride;
        uint16_t *out = rgb + row * out_w;

        for (col = 0; col < out_w; col++) {
            int16_t yy = yr[col];
            int16_t uu = ur[col / 2];
            int16_t vv = vr[col / 2];

            /* BT.601 full-range (JPEG) */
            int16_t r = yy + ((1436 * (vv - 128)) >> 10);
            int16_t g = yy - ((352 * (uu - 128) + 731 * (vv - 128)) >> 10);
            int16_t b = yy + ((1812 * (uu - 128)) >> 10);

            r = clamp8(r);
            g = clamp8(g);
            b = clamp8(b);

            out[col] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        }
    }
}

/* ========== 硬件 JPEG 解码 + 缩放到 YUV 缓冲区 ========== */

static int jpg_decode_to_yuv_buf(uint32_t jpg_addr, uint32_t jpg_w, uint32_t jpg_h,
                                  uint32_t yuv_buf, uint32_t out_w, uint32_t out_h)
{
    struct scale_device *scale_dev;
    struct jpg_device *jpg_dev;
    uint32_t aligned_w;
    uint8_t *scaler_line_buf;
    int timeout;

    scale_dev = (struct scale_device *)dev_get(HG_SCALE2_DEVID);
    jpg_dev   = (struct jpg_device *)dev_get(HG_JPG1_DEVID);

    if (!scale_dev || !jpg_dev) {
        os_printf("[img_dec] hw device not available\r\n");
        return -1;
    }

    aligned_w = ((out_w + 3) / 4) * 4;

    /* 使用 custom_malloc (SRAM 池) 替代 os_malloc (系统堆)
     * 系统堆 freemem 紧张 (3~13KB)，scaler_line_buf ~7KB 容易失败
     * SRAM 池有 ~18KB 空闲，足够容纳 */
    scaler_line_buf = (uint8_t *)custom_malloc(32 + aligned_w * 2 +
                     (32 * SRAMBUF_WLEN * 4 * 3) / 2);
    if (!scaler_line_buf) {
        os_printf("[img_dec] scaler line buf alloc failed\r\n");
        return -1;
    }

    scale_close(scale_dev);
    scale_set_in_out_size(scale_dev, jpg_w, jpg_h, out_w, out_h);
    scale_set_step(scale_dev, jpg_w, jpg_h, out_w, out_h);
    scale_set_out_yaddr(scale_dev, yuv_buf);
    scale_set_out_uaddr(scale_dev, yuv_buf + aligned_w * out_h);
    scale_set_out_vaddr(scale_dev, yuv_buf + aligned_w * out_h + aligned_w * out_h / 4);
    scale_set_line_buf_addr(scale_dev, (uint32_t)scaler_line_buf);
    scale_set_srambuf_wlen(scale_dev, SRAMBUF_WLEN);
    scale_set_start_addr(scale_dev, 0, 0);
    jpg_decode_target(jpg_dev, 1);
    scale_open(scale_dev);

    jpg_decode_photo(jpg_dev, jpg_addr);

    timeout = 3000;
    while (!jpg_is_idle(jpg_dev) && timeout > 0) {
        os_sleep_ms(1);
        timeout--;
    }

    scale_close(scale_dev);
    custom_free(scaler_line_buf);

    if (timeout <= 0) {
        os_printf("[img_dec] decode timeout!\r\n");
        return -1;
    }

    return 0;
}

/* ========== 公共接口：从内存缓冲区解码 ========== */

int img_decode_jpeg_mem(const uint8_t *jpeg_data, uint32_t jpeg_len,
                         uint8_t *rgb565_out,
                         uint32_t out_w, uint32_t out_h)
{
    uint32_t photo_size[2];
    uint32_t jpg_w, jpg_h, aligned_w, yuv_size;
    uint8_t *yuv_buf;

    /* 1. 解析 JPEG 原始尺寸 */
    photo_size[0] = 0;
    photo_size[1] = 0;
    jpg_analyze_ex(jpeg_data, photo_size);
    jpg_w = photo_size[1];
    jpg_h = photo_size[0];

    if (jpg_w == 0 || jpg_h == 0) {
        os_printf("[img_dec] invalid jpeg dims\r\n");
        return -1;
    }

    /* 2. 分配 YUV420 缓冲区 (PSRAM) */
    aligned_w = ((out_w + 3) / 4) * 4;
    yuv_size = aligned_w * out_h + aligned_w * out_h / 2;
    yuv_buf = (uint8_t *)custom_malloc_psram(yuv_size);
    if (!yuv_buf) {
        os_printf("[img_dec] yuv buf alloc %u failed\r\n", yuv_size);
        return -1;
    }

    /* 3. 硬件 JPEG 解码 + Scale2 缩放 */
    if (jpg_decode_to_yuv_buf((uint32_t)jpeg_data, jpg_w, jpg_h,
                               (uint32_t)yuv_buf, out_w, out_h) != 0) {
        custom_free_psram(yuv_buf);
        return -1;
    }

    /* 4. YUV420 → RGB565 (软件转换) */
    yuv420_to_rgb565(yuv_buf,
                      yuv_buf + aligned_w * out_h,
                      yuv_buf + aligned_w * out_h + aligned_w * out_h / 4,
                      (uint16_t *)rgb565_out,
                      out_w, out_h, aligned_w);

    custom_free_psram(yuv_buf);

    return 0;
}

/* ========== 公共接口：从 SD 卡文件解码 ========== */

#if FS_EN

#include "fatfs/ff.h"

int album_decode_jpeg_file(const char *filepath,
                           uint8_t *rgb565_out,
                           uint32_t out_w, uint32_t out_h)
{
    FIL fp;
    FRESULT res;
    uint32_t fsize, br;
    uint8_t *jpg_buf;
    int ret;

    /* 1. 打开文件 */
    res = f_open(&fp, filepath, FA_READ);
    if (res != FR_OK) {
        os_printf("[img_dec] open %s err=%d\r\n", filepath, res);
        return -1;
    }
    fsize = f_size(&fp);
    if (fsize == 0 || fsize > 512 * 1024) {
        os_printf("[img_dec] bad size %u\r\n", fsize);
        f_close(&fp);
        return -1;
    }

    /* 2. 读取 JPEG 到 PSRAM */
    jpg_buf = (uint8_t *)custom_malloc_psram(fsize);
    if (!jpg_buf) {
        os_printf("[img_dec] jpg buf alloc %u failed\r\n", fsize);
        f_close(&fp);
        return -1;
    }
    res = f_read(&fp, jpg_buf, fsize, &br);
    f_close(&fp);
    if (res != FR_OK || br != fsize) {
        os_printf("[img_dec] read err res=%d br=%u/%u\r\n", res, br, fsize);
        custom_free_psram(jpg_buf);
        return -1;
    }

    /* 3. 调用内存解码 */
    ret = img_decode_jpeg_mem(jpg_buf, fsize, rgb565_out, out_w, out_h);
    custom_free_psram(jpg_buf);
    return ret;
}

int album_decode_avi_thumbnail(const char *filepath,
                               uint8_t *rgb565_out,
                               uint32_t out_w, uint32_t out_h)
{
    FIL fp;
    FRESULT res;
    uint32_t fsize, br, pos;
    uint8_t buf[8];
    uint32_t chunk_size;
    uint8_t *jpg_buf;
    int ret;

    res = f_open(&fp, filepath, FA_READ);
    if (res != FR_OK) {
        os_printf("[img_dec] avi open %s err=%d\r\n", filepath, res);
        return -1;
    }

    fsize = f_size(&fp);

    /* Scan for first 00dc chunk (video frame) */
    pos = 0;
    while (pos + 8 < fsize) {
        f_lseek(&fp, pos);
        res = f_read(&fp, buf, 8, &br);
        if (res != FR_OK || br < 8) break;

        chunk_size = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24);

        /* 00dc = MJPEG video frame */
        if (buf[0] == 0x30 && buf[1] == 0x30 &&
            buf[2] == 0x64 && buf[3] == 0x63) {
            if (chunk_size > 12 && chunk_size < 512 * 1024) {
                jpg_buf = (uint8_t *)custom_malloc_psram(chunk_size);
                if (jpg_buf) {
                    f_read(&fp, jpg_buf, chunk_size, &br);
                    f_close(&fp);
                    /* Verify JPEG SOI marker */
                    if (br >= 2 && jpg_buf[0] == 0xFF && jpg_buf[1] == 0xD8) {
                        ret = img_decode_jpeg_mem(jpg_buf, br, rgb565_out, out_w, out_h);
                    } else {
                        os_printf("[img_dec] avi no SOI: %02X %02X\r\n",
                                  br >= 1 ? jpg_buf[0] : 0, br >= 2 ? jpg_buf[1] : 0);
                        ret = -1;
                    }
                    custom_free_psram(jpg_buf);
                    return ret;
                }
            }
            break;
        }

        /* RIFF / LIST: container chunks — skip 12-byte header (id+size+subtype) */
        if ((buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F') ||
            (buf[0] == 'L' && buf[1] == 'I' && buf[2] == 'S' && buf[3] == 'T')) {
            pos += 12;
            continue;
        }

        /* Other chunks: skip (4-byte id + 4-byte size + aligned data) */
        pos += 8 + ((chunk_size + 1) & ~1);
    }

    f_close(&fp);
    os_printf("[img_dec] avi no 00dc found\r\n");
    return -1;
}

#endif /* FS_EN */
