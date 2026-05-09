#ifndef __UI_OCR_H__
#define __UI_OCR_H__

#include "ui_manager.h"

/* 识别模式 */
#define OCR_MODE_TEXT   0
#define OCR_MODE_OBJECT 1

/* 注册拍图识字/识物页面 */
void ui_ocr_register(void);

/* 设置文件模式：传入文件路径，跳过摄像头直接识别 */
void ui_ocr_set_file(const char *filepath, uint8_t mode);

#endif /* __UI_OCR_H__ */
