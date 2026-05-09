
#include "sys_config.h"
#include "osal/task.h"
#include "osal/string.h"
#include "printer_hal.h"
#include "printer_demo.h"
#include <math.h>

/* --- 常量配置 --- */
#define PRINTER_W            384
#define PRINTER_H            512
#define PRINTER_BUF_SIZE     (PRINTER_W * PRINTER_H)
#define HEAT_TIMES_THRESHOLD 128

#define NTC_R25              30000.0f
#define NTC_B_VALUE          3950.0f
#define NTC_T25              298.15f
#define V_REF                3300.0f
#define PULLUP_RESISTOR      30000.0f

#define SEGMENTS_PER_LINE    4
#define BYTES_PER_SEGMENT    (48 / SEGMENTS_PER_LINE)
#define GRAY_LEVELS          64

#define STEPS_PER_LINE       2
#define BYTES_PER_STEP       6
#define MOTOR_LINE_BUF_LEN   (STEPS_PER_LINE * BYTES_PER_STEP)
#define SPI_TX_BUF_LEN       48
#define EXTRA_FEED_LINES_DEF 50

/* --- 外部变量 --- */
extern const unsigned char printer_demo[PRINTER_BUF_SIZE];
volatile uint8  printer_action = 0;

struct os_task handle_printer_task;

/* --- App层专属结构体封装 --- */
typedef struct {
    // 状态标志位
    volatile uint8_t is_working;        // 是否在工作
    volatile uint8_t need_motor_run;    // 走纸标志
    volatile uint8_t line_heating_done; // 本行加热完成
    volatile uint8_t pre_heat_flag;     // 预热标志

    // 运行参数
    uint32_t remaining_lines;           // 剩余行数
    uint32_t current_segment;           // 当前分段
    uint32_t current_gray_pass;         // 当前灰度
    uint32_t extra_feed_lines;          // 吐白纸行数
    uint32_t heat_count;                // 加热计数
} printer_state_t;

// 缓存区静态分配 (包含对齐和特定区段分配)
static uint8_t s_motor_buf[MOTOR_LINE_BUF_LEN] __attribute__ ((aligned(4)));
static uint8_t s_print_buf[PRINTER_BUF_SIZE] __attribute__ ((aligned(4),section(".psram.src")));
static uint8_t s_tx_buf[SPI_TX_BUF_LEN] __attribute__ ((aligned(4)));

// 数据管理结构体，存放状态和缓存指针
typedef struct {
    printer_state_t state;   // 状态机变量
    const uint8_t *src_img;	 // 源图像指针
    uint8_t *motor_buf;		 // 电机控制缓冲区
    uint8_t *print_buf;		 // 打印数据缓冲区（64级灰度映射）
    uint8_t *tx_buf;		 // SPI传输缓冲区
} printer_app_t;

// 唯一的应用层全局实例
static printer_app_t g_app = {
    .state = { .is_working = 0 },
    .src_img = NULL,
    .motor_buf = s_motor_buf,
    .print_buf = s_print_buf,
    .tx_buf = s_tx_buf
};

/* --- 算法与计算 --- */
float get_real_temperature(uint32_t adc_mv) {
    if (adc_mv == 0) return 150.0f;
    if (adc_mv >= V_REF) return -50.0f;
    float r_ntc = PULLUP_RESISTOR * (float)adc_mv / (V_REF - (float)adc_mv);
    float temp_k = 1.0f / ((1.0f / NTC_T25) + (1.0f / NTC_B_VALUE) * log(r_ntc / NTC_R25));
    return temp_k - 273.15f;
}

void enhanceContrast(uint8_t *image) {
    int minLevel = 63, maxLevel = 0;
    for (int i = 0; i < PRINTER_BUF_SIZE; i++) {
        if (image[i] < minLevel) minLevel = image[i];
        if (image[i] > maxLevel) maxLevel = image[i];
    }
    int dynamicRange = maxLevel - minLevel;
    if (dynamicRange == 0) dynamicRange = 1;
    for (int i = 0; i < PRINTER_BUF_SIZE; i++) {
        image[i] = (image[i] - minLevel) * 63 / dynamicRange;
        if (image[i] < 0) image[i] = 0;
        if (image[i] > 63) image[i] = 63;
    }
}

unsigned char restrain_int(int input) {
    if (input < 0x00) return 0x00;
    if (input > 0xFF) return 0xFF;
    return input;
}

void dithering(uint8_t *image, int width, int height) {
    for(int y = 0; y < height; y++) {
        for(int x = 0; x < width; x++) {
            int index = y * width + x;
            unsigned char oldPixel = image[index];
            unsigned char newPixel = (oldPixel < 128) ? 0 : 255;
            int error = oldPixel - newPixel;
            image[index] = newPixel;
            if(x < width - 1) image[index + 1] = restrain_int(image[index + 1] + (error * 7 / 16));
            if(y < height - 1) image[index + width] = restrain_int(image[index + width] + (error * 5 / 16));
            if(x < width - 1 && y < height - 1) image[index + width + 1] = restrain_int(image[index + width + 1] + (error * 1 / 16));
            if(x > 0 && y < height - 1) image[index + width - 1] = restrain_int(image[index + width - 1] + (error * 3 / 16));
        }
    }
}

/* 电机步进数据生成*/
void init_motor_line_buf(void) {
    static const uint8_t fullstep_table[4] = { 0x55, 0x99, 0xAA, 0x66 };
    static uint8_t current_step = 0;  // 静态变量保持相位记忆

    for (int step = 0; step < STEPS_PER_LINE; step++) {  // STEPS_PER_LINE 步/行
        uint8_t phase = fullstep_table[current_step];
        for (int i = 0; i < BYTES_PER_STEP; i++) {		 // BYTES_PER_STEP 字节/步
            g_app.motor_buf[step * BYTES_PER_STEP + i] = phase;  // 重复填充
        }
        current_step = (current_step + 1) % 4;		// 循环相位
    }
}

uint32_t app_printer_prepare_next_line(void){
    if(g_app.state.remaining_lines == 0) return 0;

    uint32_t offset_h = (g_app.state.remaining_lines - 1) * PRINTER_W;
    int start_byte = g_app.state.current_segment * BYTES_PER_SEGMENT;

    for (int j = 0; j < BYTES_PER_SEGMENT; j++) {
        g_app.tx_buf[start_byte + j] = 0;
    }

    for (int j = 0; j < BYTES_PER_SEGMENT; j++) {
        int byte_idx = start_byte + j;
        for (int bit = 0; bit < 8; bit++) {
            int pixel_idx = byte_idx * 8 + bit;
            if (g_app.print_buf[offset_h + pixel_idx] > g_app.state.current_gray_pass) {
                g_app.tx_buf[byte_idx] |= (1 << (7 - bit));
            }
        }
    }

    hal_printer_set_stb(1);
    hal_printer_spi_data_tx_dma(g_app.tx_buf, SPI_TX_BUF_LEN);

    g_app.state.current_segment++;
    if (g_app.state.current_segment >= SEGMENTS_PER_LINE) {
        g_app.state.current_segment = 0;
        g_app.state.current_gray_pass++;
        if (g_app.state.current_gray_pass >= GRAY_LEVELS) {
            g_app.state.current_gray_pass = 0;
            g_app.state.remaining_lines--;
            return 2;
        }
    }
    return 1;
}

void start_line_heating(void) {
    if (g_app.state.remaining_lines > 0) {
        g_app.state.line_heating_done = 0;
        app_printer_prepare_next_line();
    } else {
        hal_printer_set_stb(0);
        g_app.state.extra_feed_lines = EXTRA_FEED_LINES_DEF;
        init_motor_line_buf();
        hal_printer_moto_tx(g_app.motor_buf, MOTOR_LINE_BUF_LEN);
    }
}


// SPI0 回调
void app_spi0_irq_cb(uint32_t irq, uint32_t irq_data) {
    g_app.state.heat_count++;

    hal_printer_set_stb(0);
    hal_printer_set_latch(0);
    hal_printer_delay_us(2);
    hal_printer_set_latch(1);

    if (!g_app.state.line_heating_done) {
        uint32_t ret = app_printer_prepare_next_line();
        if (ret == 2) {
            g_app.state.line_heating_done = 1;
            g_app.state.need_motor_run = 1;
        }
    }
    if(g_app.state.heat_count >= HEAT_TIMES_THRESHOLD) g_app.state.heat_count = 0;
}

// Timer0 回调
void app_stb_timer_cb(uint32_t cb_data, uint32_t irq_flag) {
    hal_printer_set_stb(0);
    g_app.state.pre_heat_flag = 1;
}

/* --- 主任务 --- */
bool printer_work_isr(void) {
    uint32_t grep_step = 0;
    os_printf("[printer_task] thread started, waiting for action...\r\n");
    os_sleep_ms(1000);

    while(1) {
        if(printer_action == 0 || g_app.state.is_working == 1) {
            os_sleep_ms(50);
            continue;
        }

        os_printf("[printer_task] action=%u triggered! starting print job...\r\n", printer_action);
        // 等待视频流/H264解码器释放硬件资源（DMA、中断）
        os_sleep_ms(3000);
        // 初始化
        printer_action = 0;
        g_app.state.is_working = 1;
        g_app.state.pre_heat_flag = 0;
        g_app.state.remaining_lines = PRINTER_H;
        if (g_app.src_img == NULL) {
            g_app.src_img = printer_demo;  // 无外部图片时使用 demo
        }

		//1、硬件初始化、检查纸张/温度
        hal_printer_io_init();
        hal_printer_power_en(1);  // 使能 7.4V VH 电源
        os_sleep_ms(500); 		  // 等待电源稳定

        // 检查纸张
        if(hal_printer_is_paper_inserted()) {
            hal_printer_spi_moto_close();
            hal_printer_io_deinit();
            hal_printer_power_en(0);
            os_printf("--> Printer error...no paper!!!\r\n");
            g_app.state.is_working = 0;
            continue;
        }
        os_printf("[printer_task] paper ok\r\n");

        // 检查温度
        float current_temp = get_real_temperature((uint32_t)hal_printer_get_tm());
        os_printf("[printer_task] temp=%.1f\r\n", current_temp);
        if (current_temp >= 70.0f) {
            hal_printer_spi_moto_close();
            hal_printer_io_deinit(); hal_printer_power_en(0);
            g_app.state.is_working = 0;
            os_sleep_ms(2000);
            continue;
        }

        // 2、图像处理
        os_printf("[printer_task] img proc start\r\n");
        uint8_t min_pixel = 255;
        uint8_t max_pixel = 0;
        const uint8_t *src_ptr = g_app.src_img;

        for (int i = 0; i < PRINTER_BUF_SIZE; i++) {
            uint8_t p = *src_ptr++;
            if (p < min_pixel) min_pixel = p;
            if (p > max_pixel) max_pixel = p;
        }

        int dynamic_range = max_pixel - min_pixel;
        if (dynamic_range == 0) dynamic_range = 1;
        os_printf("[printer_task] range %u-%u\r\n", min_pixel, max_pixel);

        uint8_t heat_lut[256];
        for (int i = 0; i < 256; i++) {
            if (i <= min_pixel) {
                heat_lut[i] = 63;
            } else if (i >= max_pixel) {
                heat_lut[i] = 0;
            } else {
                heat_lut[i] = 63 - ((i - min_pixel) * 63 / dynamic_range);
            }
        }

        src_ptr = g_app.src_img;
        uint8_t *dst_base = g_app.print_buf;

        for (int y = 0; y < PRINTER_H; y++) {
            uint8_t *dst_ptr = dst_base + (y * PRINTER_W) + (PRINTER_W - 1);
            for (int x = 0; x < PRINTER_W; x++) {
                *dst_ptr-- = heat_lut[*src_ptr++];
            }
        }
        os_printf("[printer_task] lut+mirror done\r\n");

        // 状态复位
        g_app.state.current_gray_pass = 0;
        g_app.state.current_segment = 0;
        memset(g_app.tx_buf, 0x00, SPI_TX_BUF_LEN);
        os_printf("[printer_task] spi clear\r\n");
        hal_printer_spi_data_tx_dma(g_app.tx_buf, SPI_TX_BUF_LEN);
        hal_printer_spi_data_wait_tx_done();
        hal_printer_spi_data_clear_pending();
        hal_printer_set_latch(0);
        hal_printer_set_latch(1);
        os_printf("[printer_task] preheat\r\n");

        // 3、Timer0 单独预热
        void *stb_timer0 = hal_printer_stb_timer_open();
        if (stb_timer0) {
            hal_printer_stb_timer_start(stb_timer0, 1, app_stb_timer_cb);
            uint32_t preheat_timeout = 200;
            while(g_app.state.pre_heat_flag == 0 && preheat_timeout > 0) {
                os_sleep_ms(10);
				preheat_timeout--;
            }
            hal_printer_stb_timer_close(stb_timer0);
        }

        // 挂载全局同步中断
        os_printf("[printer_task] motor+irq\r\n");
        hal_printer_moto_en(1);
        hal_printer_spi_data_register_irq_cb(app_spi0_irq_cb, NULL);

        // 串行模式变量初始化
        g_app.state.remaining_lines = PRINTER_H;
        g_app.state.current_gray_pass = 0;
        g_app.state.current_segment = 0;
        g_app.state.extra_feed_lines = EXTRA_FEED_LINES_DEF;
        g_app.state.need_motor_run = 0;
        g_app.state.line_heating_done = 0;

        // 启动第一行加热
        os_printf("[printer_task] heating start\r\n");
        app_printer_prepare_next_line();

        // 阻塞等待任务完成
        while(g_app.state.is_working == 1) {
            if (g_app.state.need_motor_run == 1) {
                g_app.state.need_motor_run = 0;

                init_motor_line_buf();
                hal_printer_moto_tx(g_app.motor_buf, MOTOR_LINE_BUF_LEN);

                if (g_app.state.remaining_lines > 0) {
                    start_line_heating();
                } else {
                    hal_printer_set_stb(0);
                    if (g_app.state.extra_feed_lines > 0) {
                        g_app.state.extra_feed_lines--;
                        g_app.state.need_motor_run = 1;
                    } else {
                        g_app.state.is_working = 0;
                    }
                }
            } else {
                os_sleep_ms(2);
            }
        }

        // 收尾清理
        hal_printer_spi_moto_close();
        hal_printer_io_deinit();
        hal_printer_power_en(0);
        os_printf("[printer_task] print job done, cleanup complete\r\n");
    }
}

void printer_thread_init() {
    OS_TASK_INIT("handle_printer", &handle_printer_task, printer_work_isr, NULL, OS_TASK_PRIORITY_ABOVE_NORMAL, 1024);
}

void printer_set_picture_addr(const uint8_t *addr) {
    if (addr) {
        g_app.src_img = addr;
    }
}
