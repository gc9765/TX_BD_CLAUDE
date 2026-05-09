#ifndef _PRINTER_HAL_H_
#define _PRINTER_HAL_H_

#include "typesdef.h"

/* 打印机硬件初始化与反初始化 */
void hal_printer_io_init(void);
void hal_printer_io_deinit(void);

/* 电源与互斥设备控制 */
void hal_printer_power_en(uint8_t en);          // 打印机整机使能与DVP互斥切换
void hal_printer_moto_sleep_en(uint8_t sleep);  // 电机休眠控制

/* 传感器读取 */
int32_t hal_printer_get_tm(void);               // 读取热敏电阻温度
uint8_t hal_printer_is_paper_inserted(void);    // 检测是否缺纸

/* 基础IO控制 */
void hal_printer_set_latch(uint8_t level);      // 控制锁存引脚 (LATCH)
void hal_printer_set_stb(uint8_t on);           // 控制加热引脚 (STB PWM或普通高低电平)

/* SPI 数据传输 */
// 发送打印点阵数据
void hal_printer_spi_data_tx_dma(uint8_t *data, uint32_t len);
// 等待打印数据DMA传输完成 (阻塞)
void hal_printer_spi_data_wait_tx_done(void);
// 清除打印数据DMA挂起状态
void hal_printer_spi_data_clear_pending(void);
// 注册数据SPI发送完成中断回调
void hal_printer_spi_data_register_irq_cb(void (*cb)(uint32_t, uint32_t), void *irq_data);
// 释放数据SPI中断
void hal_printer_spi_data_release_irq(void);

/* SPI 电机控制传输 (Quad-SPI) - 旧方式,已被PWM控制替代 */
// 发送步进电机相位数据
void hal_printer_spi_moto_tx_dma(uint8_t *data, uint32_t len);
// 注册电机SPI发送完成中断回调
void hal_printer_spi_moto_register_irq_cb(void (*cb)(uint32_t, uint32_t), void *irq_data);
// 关闭电机SPI
void hal_printer_spi_moto_close(void);

/* STB 预加热定时器 */
void* hal_printer_stb_timer_open(void);
void hal_printer_stb_timer_close(void* timer_dev);
// 启动预热定时器并注册回调
void hal_printer_stb_timer_start(void* timer_dev, uint32_t ms, void (*cb)(uint32_t, uint32_t));

/* 延时接口 */
void hal_printer_delay_us(uint32_t us);

/* ======================================================= */
/* PWM定时器电机控制 (使用Timer1)                          */
/* ======================================================= */

// 初始化PWM电机控制 (GPIO + Timer1)
void hal_printer_motor_pwm_init(void);

// 启动电机走指定步数 (带加速曲线)
void hal_printer_motor_move(uint32_t steps);

// 停止电机
void hal_printer_motor_stop(void);

// 检查电机是否正在运行
uint8_t hal_printer_motor_is_busy(void);

// 注册电机运动完成回调
void hal_printer_motor_register_done_cb(void (*cb)(void));

#endif