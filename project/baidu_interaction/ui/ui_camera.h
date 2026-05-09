#ifndef __UI_CAMERA_H__
#define __UI_CAMERA_H__

#include "ui_manager.h"

/* 注册照相机页面（预览 + 拍照存SD卡） */
void ui_camera_register(void);

/* 拍照回调 — 设置后拍照数据传给回调，不存SD卡 */
typedef void (*camera_photo_cb_t)(const uint8_t *jpeg_data, uint32_t jpeg_len);
void camera_set_photo_cb(camera_photo_cb_t cb);

/* 摄像头流管理（供 OCR 等页面复用） */
void camera_start_streams(void);
void camera_stop_streams(void);
void camera_stop_preview(void);  /* 仅停止预览流，冻结LCD显示 */

/* 拍照（JPEG编码 + 快门音效 + 定时器轮询） */
void camera_take_photo(void);
int  camera_is_capturing(void);

/* 清理拍照状态（定时器 + JPEG编码） */
void camera_cleanup_photo(void);

/* 防花屏遮罩 */
void camera_create_splash(lv_obj_t *parent);
void camera_cleanup_splash(void);

/* 闪屏特效 */
void camera_flash_effect(lv_obj_t *parent);

#endif /* __UI_CAMERA_H__ */
