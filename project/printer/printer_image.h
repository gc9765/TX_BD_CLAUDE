#ifndef __PRINTER_IMAGE_H__
#define __PRINTER_IMAGE_H__

#include <stdint.h>
#include <stddef.h>

/**
 * @brief 将 JPEG 图片数据解码、缩放到 384x512 灰度，并触发打印机打印
 *
 * 使用增强版 JPEG 尺寸解析（支持 SOF0/SOF1/SOF2），硬件 JPEG 解码器
 * 将 JPEG 解码为 YUV，硬件缩放器缩放到 384x512，
 * Y 平面即为灰度图像，直接用作打印机源图像。
 *
 * @param jpeg_data JPEG 数据指针（来自 BRTC onVideoData 回调）
 * @param len       JPEG 数据长度（字节）
 * @return 0=成功并已触发打印，-1=失败
 */
int printer_image_from_jpeg(const uint8_t *jpeg_data, size_t len);

#endif /* __PRINTER_IMAGE_H__ */
