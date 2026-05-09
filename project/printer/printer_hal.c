#include "printer_hal.h"
#include "sys_config.h"
#include "dev.h"
#include "devid.h"
#include "hal/gpio.h"
#include "hal/dvp.h"
#include "hal/vpp.h"
#include "hal/adc.h"
#include "hal/spi.h"
#include "hal/pwm.h"
#include "hal/timer_device.h"
#include "osal/irq.h"
#include "osal/string.h"

/* --- 引脚与参数宏 --- */
#define  MOTO_SLEEP_PIN      PB_7   // LOW sleep, HIGH run
#define  PRINTER_LATCH_PIN   PB_8   // 锁存信号
#define  PRINTER_TM_PIN      PC_5   // 温度传感器 ADC
#define  PRINTER_PSENSOR_PIN PC_6   // 纸张传感器
#define  PRINTER_STB_PIN     PB_10  // 选通信号
#define  PRINTER_EN_PIN      PC_7   // VH电源使能

#define SPI_CLK_FREQ         5300000
#define SPI_MOTO_CLK         1500
#define STB_PIN_PWM_PERIOD   1000            
#define STB_PIN_PWM_DUTY     900 

/* --- HAL层专属结构体封装 --- */
typedef struct {
    struct hgpwm_v0 *pwm;
    struct hgadc_v0 *adc;
    struct spi_device *spi0;
    void (*motor_done_cb)(uint32_t, uint32_t);
} printer_hal_hw_t;


static printer_hal_hw_t g_hw = {0};

void hal_printer_io_init(void) {
    gpio_iomap_output(MOTO_SLEEP_PIN, GPIO_IOMAP_OUTPUT);
    gpio_set_val(MOTO_SLEEP_PIN, 0);
    gpio_iomap_output(PRINTER_LATCH_PIN, GPIO_IOMAP_OUTPUT);
    gpio_set_val(PRINTER_LATCH_PIN, 1);
    gpio_iomap_output(PRINTER_STB_PIN, GPIO_IOMAP_OUTPUT);
    gpio_set_val(PRINTER_STB_PIN, 0); 
    gpio_iomap_output(PRINTER_EN_PIN, GPIO_IOMAP_OUTPUT);
    
    gpio_set_mode(PRINTER_TM_PIN, GPIO_PULL_NONE, GPIO_PULL_LEVEL_NONE);
    gpio_iomap_input(PRINTER_TM_PIN, GPIO_IOMAP_INPUT);
    gpio_set_mode(PRINTER_PSENSOR_PIN, GPIO_PULL_NONE, GPIO_PULL_LEVEL_NONE);
    gpio_iomap_input(PRINTER_PSENSOR_PIN, GPIO_IOMAP_INPUT);

    g_hw.spi0 = (struct spi_device *)dev_get(HG_SPI0_DEVID);
    spi_ioctl(g_hw.spi0, SPI_CFG_SET_NONE_CS, 1, 0);
    spi_ioctl(g_hw.spi0, SPI_SET_LSB_FIRST, 0, 0); //低位先行
    spi_ioctl(g_hw.spi0, SPI_KICK_DMA_EN, 0, 0);
    spi_open(g_hw.spi0, SPI_CLK_FREQ, SPI_MASTER_MODE, SPI_WIRE_SINGLE_MODE, SPI_CPOL_0_CPHA_0);
    spi_release_irq(g_hw.spi0, SPI_IRQ_FLAG_TX_DONE | SPI_IRQ_FLAG_RX_DONE);

    // GPIO 电机引脚初始化
    gpio_set_mode(PB_14, GPIO_PULL_NONE, GPIO_PULL_LEVEL_NONE);
    gpio_iomap_output(PB_14, GPIO_IOMAP_OUTPUT); // AIN1 
    gpio_set_mode(PB_15, GPIO_PULL_NONE, GPIO_PULL_LEVEL_NONE);
    gpio_iomap_output(PB_15, GPIO_IOMAP_OUTPUT); // AIN2 
    gpio_set_mode(PC_1, GPIO_PULL_NONE, GPIO_PULL_LEVEL_NONE);
    gpio_iomap_output(PC_1, GPIO_IOMAP_OUTPUT);  // BIN1 
    gpio_set_mode(PC_0, GPIO_PULL_NONE, GPIO_PULL_LEVEL_NONE);
    gpio_iomap_output(PC_0, GPIO_IOMAP_OUTPUT);  // BIN2 
    
    gpio_set_val(PB_14, 0);
    gpio_set_val(PB_15, 0);
    gpio_set_val(PC_1, 0);
    gpio_set_val(PC_0, 0);

}

void hal_printer_io_deinit(void) {
    g_hw.adc = (struct hgadc_v0*)dev_get(HG_ADC0_DEVID);
    adc_close((struct adc_device *)(g_hw.adc));
    adc_delete_channel((struct adc_device *)(g_hw.adc), PRINTER_TM_PIN);

    spi_close(g_hw.spi0);
}

void hal_printer_power_en(uint8_t en) {


    if(en) {
        gpio_set_val(PRINTER_EN_PIN, 1);
    } else {
        gpio_set_val(PRINTER_EN_PIN, 0);
    }
}

void hal_printer_delay_us(uint32_t us) {
    volatile uint32_t i = 20 * us;
    while(i--) {}
}

void hal_printer_moto_en(uint8_t sleep) {
    if(sleep) {
        gpio_set_val(MOTO_SLEEP_PIN, 1);
        os_sleep_ms(5);
    } else {
        gpio_set_val(MOTO_SLEEP_PIN, 0);
    }
}

int32_t hal_printer_get_tm(void) {
    uint32_t vol = 0;
    hal_printer_delay_us(100);
    g_hw.adc = (struct hgadc_v0*)dev_get(HG_ADC0_DEVID);
    adc_open((struct adc_device *)(g_hw.adc));
    adc_add_channel((struct adc_device *)g_hw.adc, PRINTER_TM_PIN);
    adc_get_value((struct adc_device *)g_hw.adc, PRINTER_TM_PIN, &vol);
    return vol;
}

uint8_t hal_printer_is_paper_inserted(void) {
    uint8_t val = gpio_get_val(PRINTER_PSENSOR_PIN);
    return (val == 0) ? 1 : 0; 
}

void hal_printer_set_latch(uint8_t level) {
    gpio_set_val(PRINTER_LATCH_PIN, level);
}

void hal_printer_set_stb(uint8_t on) {
    gpio_set_val(PRINTER_STB_PIN, on);
}

void hal_printer_moto_tx(uint8_t *data, uint32_t len) {
    for(uint32_t i = 0; i < len; i++) {
        uint8_t phase = data[i];
        
        gpio_set_val(PB_14, (phase & 0x80) ? 1 : 0); 
        gpio_set_val(PB_15, (phase & 0x40) ? 1 : 0); 
        gpio_set_val(PC_1,  (phase & 0x20) ? 1 : 0); 
        gpio_set_val(PC_0,  (phase & 0x10) ? 1 : 0); 
        
        if (i < 2) {
            hal_printer_delay_us(2000); 
        } else if (i < 5) {
            hal_printer_delay_us(1500); 
        } else {
            hal_printer_delay_us(1000); 
        }
    }

    if (g_hw.motor_done_cb) {
        g_hw.motor_done_cb(0, 0); 
    }
}

void hal_printer_spi_moto_close(void) {
    gpio_set_val(PB_14, 0);
    gpio_set_val(PB_15, 0);
    gpio_set_val(PC_1, 0);
    gpio_set_val(PC_0, 0);
}

void hal_printer_spi_moto_register_irq_cb(void (*cb)(uint32_t, uint32_t), void *irq_data) {
    g_hw.motor_done_cb = cb;
}

void hal_printer_spi_data_tx_dma(uint8_t *data, uint32_t len) {
    spi_ioctl(g_hw.spi0, SPI_KICK_DMA_TX, (uint32_t)data, len);
}

void hal_printer_spi_data_wait_tx_done(void) {
    while(!spi_ioctl(g_hw.spi0, SPI_GET_DMA_PENDING, 0, 0)) {}
}

void hal_printer_spi_data_clear_pending(void) {
    spi_ioctl(g_hw.spi0, SPI_CLEAR_DMA_PENDING, 1, 0);
}

void hal_printer_spi_data_register_irq_cb(void (*cb)(uint32_t, uint32_t), void *irq_data) {
    spi_request_irq(g_hw.spi0, SPI_IRQ_FLAG_TX_DONE, cb, irq_data);
}

void hal_printer_spi_data_release_irq(void) {
    spi_release_irq(g_hw.spi0, SPI_IRQ_FLAG_TX_DONE);
}

void* hal_printer_stb_timer_open(void) {
    struct timer_device *stb_timer0 = (struct timer_device *)dev_get(HG_TIMER0_DEVID);
    if (!stb_timer0) return NULL;
    int32 ret = timer_device_open(stb_timer0, TIMER_TYPE_ONCE, 0);
    if (ret != 0) return NULL;
    return stb_timer0;
}

void hal_printer_stb_timer_close(void* timer_dev) {
    timer_device_close((struct timer_device *)timer_dev);
}

void hal_printer_stb_timer_start(void* timer_dev, uint32_t ms, void (*cb)(uint32_t, uint32_t)) {
    if (!timer_dev) return;
    hal_printer_set_stb(1);
    timer_device_start((struct timer_device *)timer_dev, ms * 240000, cb, 0);
}