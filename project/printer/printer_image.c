#include "sys_config.h"
#include "typesdef.h"
#include "dev.h"
#include "devid.h"
#include "osal/string.h"
#include "hal/scale.h"
#include "hal/jpeg.h"
#include "dev/jpg/hgjpg.h"
#include "lib/lcd/lcd.h"
#include "custom_mem/custom_mem.h"
#include "printer_image.h"
#include "printer_driver.h"

/* 打印机目标尺寸 */
#define PRINT_W  384
#define PRINT_H  512

/* 打印机专用源图像缓冲区，独立于视频子系统的 vga_room，避免读写冲突 */
static uint8_t s_printer_src[PRINT_W * PRINT_H] __attribute__ ((aligned(4), section(".psram.src")));

/* 外部 PSRAM 缓冲区（视频子系统帧缓冲，仅用于硬件解码临时输出） */
uint8_t vga_room[2][640*480+640*480/2]__attribute__ ((aligned(4),section(".psram.src")));
/* jpg_analyze 在 playback.c 中定义（仅支持 SOF0）*/

extern void jpg_analyze(uint8_t *buf, uint8_t *size);

/*
 * 增强版 JPEG 尺寸解析：支持 SOF0(0xFFC0) / SOF1(0xFFC1) / SOF2(0xFFC2)
 * 输出: size[0]=height(uint32), size[1]=width(uint32)，与 playback.c 中用法一致
 */
static void jpg_analyze_ex(uint8_t *buf, uint32_t *size)
{
    uint32_t itk;
    for (itk = 0; itk < 4096; itk++) {
        if (buf[itk] == 0xFF) {
            uint8_t marker = buf[itk + 1];
            if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
                /* SOF marker: FF Cx [len_h len_m] [precision] [height_h height_m] [width_h width_m] */
                uint8_t *p = (uint8_t *)size;
                p[0] = buf[itk + 6];  /* height LSB */
                p[1] = buf[itk + 5];  /* height MSB */
                p[2] = 0;
                p[3] = 0;
                p[4] = buf[itk + 8];  /* width LSB */
                p[5] = buf[itk + 7];  /* width MSB */
                p[6] = 0;
                p[7] = 0;
                return;
            }
            /* 跳过其他 marker 的数据段 */
            if (marker >= 0xD0 && marker <= 0xD9) continue; /* RST/SOI/EOI 无数据段 */
            if (marker == 0x00) continue;                   /* 填充字节 */
            if (marker == 0xFF) continue;                   /* 填充 */
            /* 有数据段的 marker，跳过 */
            if (itk + 3 < 4096) {
                uint16_t seg_len = (buf[itk + 2] << 8) | buf[itk + 3];
                itk += seg_len + 1; /* +1 因为 for 循环会再 itk++ */
            }
        }
    }
}

/*
 * 硬件 JPEG 解码 + 缩放到 YUV 缓冲区
 *
 * 参考 lcd.c 中的 jpg_decode_scale_config() 和 jpg_decode_to_lcd()
 * 自行配置缩放器输出地址到指定的 PSRAM 缓冲区
 */
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
        os_printf("[printer_img] hardware device not available\r\n");
        return -1;
    }

    aligned_w = ((out_w + 3) / 4) * 4;  /* 384 已经 4 对齐 */

    /* 分配 scaler line buffer（使用 OS 堆，与 lcd.c 一致） */
    scaler_line_buf = (uint8_t *)os_malloc(32 + aligned_w * 2 +
                     (32 * SRAMBUF_WLEN * 4 * 3) / 2);
    if (!scaler_line_buf) {
        os_printf("[printer_img] scaler line buffer alloc failed\r\n");
        return -1;
    }

    os_printf("[printer_img] decode %ux%u -> %ux%u (aligned_w=%u)\r\n",
              jpg_w, jpg_h, out_w, out_h, aligned_w);

    /* 配置缩放器 */
    scale_close(scale_dev);
    scale_set_in_out_size(scale_dev, jpg_w, jpg_h, out_w, out_h);
    scale_set_step(scale_dev, jpg_w, jpg_h, out_w, out_h);
    scale_set_out_yaddr(scale_dev, yuv_buf);
    scale_set_out_uaddr(scale_dev, yuv_buf + aligned_w * out_h);
    scale_set_out_vaddr(scale_dev, yuv_buf + aligned_w * out_h + aligned_w * out_h / 4);
    scale_set_line_buf_addr(scale_dev, (uint32_t)scaler_line_buf);
    scale_set_srambuf_wlen(scale_dev, SRAMBUF_WLEN);
    scale_set_start_addr(scale_dev, 0, 0);
    jpg_decode_target(jpg_dev, 1);  /* 输出到 scaler，与 lcd.c jpg_decode_scale_config 一致 */
    scale_open(scale_dev);

    /* 触发硬件 JPEG 解码（JPEG→YUV 经缩放器输出到 yuv_buf） */
    jpg_decode_photo(jpg_dev, jpg_addr);

    /* 等待解码完成（直接检查 JPEG 硬件是否空闲） */
    timeout = 3000;
    while (!jpg_is_idle(jpg_dev) && timeout > 0) {
        os_sleep_ms(1);
        timeout--;
    }

    scale_close(scale_dev);
    os_free(scaler_line_buf);

    if (timeout <= 0) {
        os_printf("[printer_img] decode timeout!\r\n");
        return -1;
    }

    os_printf("[printer_img] decode ok, Y plane at 0x%08X\r\n", yuv_buf);
    return 0;
}

int printer_image_from_jpeg(const uint8_t *jpeg_data, size_t len)
{
    uint8_t *jpg_buf;
    uint32_t photo_size[2];
    uint32_t jpg_w, jpg_h;
    uint32_t yuv_buf;
    int ret;

    if (!jpeg_data || len == 0) {
        os_printf("[printer_img] invalid input\r\n");
        return -1;
    }

    os_printf("[printer_img] step1: jpeg input len=%u\r\n", (uint32_t)len);

    /* 1. 拷贝 JPEG 到 PSRAM（硬件 DMA 需要 PSRAM 地址） */
    jpg_buf = (uint8_t *)custom_malloc_psram(len);
    if (!jpg_buf) {
        os_printf("[printer_img] psram alloc %u failed\r\n", (uint32_t)len);
        return -1;
    }
    os_printf("[printer_img] step1: psram alloc ok, buf=0x%08X\r\n", (uint32_t)jpg_buf);
    memcpy(jpg_buf, jpeg_data, len);

    /* 2. 解析 JPEG 原始尺寸（与 playback.c 中 LCD 路径用法一致） */
    photo_size[0] = 0;
    photo_size[1] = 0;
    jpg_analyze_ex(jpg_buf, photo_size);
    jpg_w = photo_size[1];   /* width  */
    jpg_h = photo_size[0];   /* height */
    os_printf("[printer_img] step2: jpeg size: %u x %u\r\n", jpg_w, jpg_h);

    if (jpg_w == 0 || jpg_h == 0) {
        os_printf("[printer_img] invalid jpeg dimensions\r\n");
        custom_free_psram(jpg_buf);
        return -1;
    }

    /* 3. 硬件解码+缩放，宽高互换以匹配打印机物理方向 */
    yuv_buf = (uint32_t)vga_room[0];
    os_printf("[printer_img] step3: hw decode %ux%u -> %ux%u (swapped)\r\n",
              jpg_w, jpg_h, PRINT_H, PRINT_W);
    ret = jpg_decode_to_yuv_buf((uint32_t)jpg_buf, jpg_w, jpg_h,
                                 yuv_buf, PRINT_H, PRINT_W);

    if (ret != 0) {
        os_printf("[printer_img] decode failed\r\n");
        custom_free_psram(jpg_buf);
        return -1;
    }

    /* 4. 转置拷贝 Y 平面：512W×384H → 384W×512H */
    os_printf("[printer_img] step4: transpose copy to s_printer_src=0x%08X\r\n",
              (uint32_t)s_printer_src);
    {
        uint8_t *yuv_bytes = (uint8_t *)yuv_buf;
        for (int row = 0; row < PRINT_W; row++) {
            for (int col = 0; col < PRINT_H; col++) {
                s_printer_src[col * PRINT_W + (PRINT_W - 1 - row)] = yuv_bytes[row * PRINT_H + col];
            }
        }
    }
    custom_free_psram(jpg_buf);
    os_printf("[printer_img] step4: transpose copy done, jpg_buf freed\r\n");

    /* 5. 设置打印机源图像并触发打印 */
    printer_set_picture_addr(s_printer_src);
    os_printf("[printer_img] step5: src_img set to 0x%08X, triggering print\r\n", (uint32_t)s_printer_src);
    printer_action = 1;

    os_printf("[printer_img] print triggered\r\n");
    return 0;
}
