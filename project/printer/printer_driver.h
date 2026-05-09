#ifndef __PRINTER_DRIVER_H__
#define __PRINTER_DRIVER_H__

#include <stdint.h>
#include <stdbool.h>

/* 外部控制标志位：写 1 触发打印任务 */
extern volatile uint8_t printer_action;

/* 打印机任务句柄（如需外部管理该任务） */
extern struct os_task handle_printer_task;

/**
 * @brief 初始化打印机主线程任务
 */
void printer_thread_init(void);

/**
 * @brief 设置要打印的图像源地址（384x512 灰度，0-255）
 * @param addr 灰度图像数据指针
 */
void printer_set_picture_addr(const uint8_t *addr);

#endif /* __PRINTER_DRIVER_H__ */