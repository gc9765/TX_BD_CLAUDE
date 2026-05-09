#ifndef UI_ALBUM_DECODE_H
#define UI_ALBUM_DECODE_H

#include "typesdef.h"

#if FS_EN

/* Decode a JPEG file to RGB565 buffer
 * filepath: full path, e.g. "0:/photo/IMG_0001.jpg"
 * rgb565_out: pre-allocated output buffer (out_w * out_h * 2 bytes)
 * out_w, out_h: target dimensions (hardware auto-aligns to 4)
 * Returns: 0 success, -1 failure
 */
int album_decode_jpeg_file(const char *filepath,
                           uint8_t *rgb565_out,
                           uint32_t out_w, uint32_t out_h);

/* Extract first MJPEG frame from AVI file and decode to RGB565 thumbnail
 * filepath: full path to .avi file
 * rgb565_out: pre-allocated output buffer (out_w * out_h * 2 bytes)
 * out_w, out_h: target dimensions
 * Returns: 0 success, -1 failure
 */
int album_decode_avi_thumbnail(const char *filepath,
                               uint8_t *rgb565_out,
                               uint32_t out_w, uint32_t out_h);

#endif /* FS_EN */

/* Decode JPEG data in memory to RGB565 buffer (no filesystem dependency)
 * jpeg_data: JPEG bitstream pointer (not modified, only read)
 * jpeg_len:  JPEG data length in bytes
 * rgb565_out: pre-allocated output buffer (out_w * out_h * 2 bytes)
 * out_w, out_h: target dimensions
 * Returns: 0 success, -1 failure
 */
int img_decode_jpeg_mem(const uint8_t *jpeg_data, uint32_t jpeg_len,
                         uint8_t *rgb565_out,
                         uint32_t out_w, uint32_t out_h);

#endif /* UI_ALBUM_DECODE_H */
