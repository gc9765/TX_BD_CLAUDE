#include "sys_config.h"
#include "typesdef.h"
#include "adkey.h"
#include "keyScan.h"
 
#include "dev/adc/hgadc_v0.h"
#include "hal/gpio.h"
#include "osal/string.h"


static void key_adkey_init(key_channel_t *key,uint8_t enable)
{
    adkey_t *adkey = (adkey_t *)key->priv;
    struct hgadc_v0 *adc = (struct hgadc_v0*)dev_get(HG_ADC0_DEVID);

    if(enable)
    {
        adc_open((struct adc_device *)adc);	
        gpio_set_mode(adkey->pin,adkey->pull,adkey->pull_level);
        adc_add_channel((struct adc_device *)adc, adkey->pin);	
        os_printf("%s:%d\n",__FUNCTION__,__LINE__);
        adkey->priv = (void*)adc;
        key->enable = 1;
    }
    else
    {
        adc_open((struct adc_device *)adc);	
        adc_delete_channel((struct adc_device *)adc, adkey->pin);
        os_printf("%s:%d\n",__FUNCTION__,__LINE__);
        adkey->priv = (void*)adc;
        key->enable = 0;
    }

	


}

static uint8 key_adkey_scan(key_channel_t *key)
{
    adkey_t *adkey = (adkey_t *)key->priv;
    uint32 vol;
    struct adkey_scan_code *key_scan = (struct adkey_scan_code*)key->key_table;
    adc_get_value((struct adc_device *)adkey->priv, adkey->pin, &vol);
	//os_printf("vol:%d\t%d\n",vol,adkey->pin);
    //记录当前adc的值,用与发送到应用层,至于应用层是否需要,由应用层去管理
    key->extern_value = vol;
    for(;;)
    {
        if(vol>=key_scan->adc)
        {
            key_scan++;
        }
        else
        {
            key_scan--;
            break;
        }
    }
    //printf("key_scan->key:%d\tvol:%d\n",key_scan->key,vol);
    return key_scan->key;
}








/*********************************************************
 *  拍学机 ADKey 参数配置 (PA3, 5键分压电路)
 *
 *  电路拓扑: VCC(3.3V) --- R_pull(30K上拉) ---+--- PA3(ADC采样)
 *                                             |
 *                                 SW1(OK)  -- 0Ω   --- GND
 *                                 SW2(上)  -- 5.1K  --- GND
 *                                 SW3(AI)  -- 13.3K --- GND
 *                                 SW4(下)  -- 30K   --- GND
 *                                 SW5(M)   -- 62K   --- GND
 *
 *  ADC = 12bit (0~4095)
 *  ADC_code = 4096 × R_btn / (R_pull + R_btn)
 *
 *  理论ADC码值:
 *    SW1(OK)  0Ω    : 4096 × 0/(30+0)     =    0
 *    SW2(上)  5.1K  : 4096 × 5.1/(30+5.1) =  595
 *    SW3(AI)  13.3K : 4096 × 13.3/(30+13.3)= 1259
 *    SW4(下)  30K   : 4096 × 30/(30+30)   = 2048
 *    SW5(M)   62K   : 4096 × 62/(30+62)   = 2761
 *    无按键   ∞     : ~4096
 *
 *  阈值 = 相邻码值中点 - 100(余量):
 *    0↔595:   298-100 ≈ 200
 *    595↔1259: 927-100 ≈ 800
 *    1259↔2048:1653-100 ≈ 1500
 *    2048↔2761:2405-100 ≈ 2300
 *    2761↔4096:3429-100 ≈ 3300
 *
 *  注意: 此为理论计算值，实际电阻有±5%误差，
 *  建议实测各按键ADC读数后微调阈值。
 *  调试方法: 取消下方 os_printf 注释，观察实际 vol 值。
 ************************************************************/
static const struct adkey_scan_code adkey_table[] =
{
	{0,     AD_PRESS},  // SW1 OK键  (0Ω)    → ADC ≈ 0
	{600,   AD_UP},     // SW2 上键  (5.1K)  → ADC ≈ 595
	{1300,  AD_A},      // SW3 AI键  (13.3K) → ADC ≈ 1259
	{2000,  AD_DOWN},   // SW4 下键  (30K)   → ADC ≈ 2048
	{2900,  AD_B},      // SW5 M键   (62K)   → ADC ≈ 2761
	{3700,  KEY_NONE},  // 无按键区域          → ADC ≈ 4096
	{4096,  KEY_NONE},
};

static const keys_t adkey_arg =
{
    .period_long     = 500,   //按键长按时间500ms
    .period_repeat   = 1000,  //按键重复时间1000ms
    .period_dither = 80,	  //按键消抖时间80ms
};


/* 拍学机: PA3 外接30K上拉电阻至3.3V，无需芯片内部上拉 */
static adkey_t adkey= {
  .priv = NULL,
  .pin  = PA_3,
  .pull = GPIO_PULL_NONE,        // 使用外部30K上拉，禁用内部上下拉
  .pull_level = GPIO_PULL_LEVEL_100K,
};



//外部调用
key_channel_t adkey_key = 
{
  .init       = key_adkey_init,
  .scan       = key_adkey_scan,
  .prepare    = NULL,
  .priv       = (void*)&adkey,
  .key_arg    = &adkey_arg,//按键的参数,可能不同的类型按键,参数不一样
  .key_table  = &adkey_table,
};



