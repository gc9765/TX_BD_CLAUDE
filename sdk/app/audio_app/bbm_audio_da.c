#include "sys_config.h"
#include "typesdef.h"
#include "list.h"
#include "dev.h"
#include "devid.h"
#include "string.h"

#include "osal/task.h"
#include "osal/semaphore.h"
#include "osal/string.h"
#include "hal/audac.h"
#include "stream_frame.h"
#include "osal_file.h"
#include "stream_frame.h"
#include "dev/audio/components/fade/aufade.h"
#include "custom_mem.h"
#include "intercom.h"

#ifdef PSRAM_HEAP
	#define ADUIO_MALLOC	os_malloc_psram
	#define ADUIO_FREE		os_free_psram
	#define ADUIO_ZALLOC    os_zalloc_psram
	#define AUDIONUM	(16)
#else
	#define ADUIO_MALLOC	os_malloc
	#define ADUIO_FREE		os_free
	#define ADUIO_ZALLOC    os_zalloc
	#define AUDIONUM	(8)
#endif

#define AUDIOLEN	320
#define CACHE_BUF_LEN   320

#define AEC_PROCESS   0

static int prev_filter_type = 0;
static stream *global_audio_dac_s = NULL;
struct audio_da_config;
typedef int32 (*audio_da_write)(struct audio_da_config *audio, void* buf, uint32 len);

int16_t copy_buf[CACHE_BUF_LEN/2] __attribute__ ((aligned(4),section(".psram.src")));
uint32_t copy_len = CACHE_BUF_LEN;

#if AEC_PROCESS == 1
int16 aecm_buf[CACHE_BUF_LEN/2] = {0};
#endif

static uint32_t empty_buf[AUDIOLEN/4];

struct audio_dac_priv {
	struct os_semaphore cache_sema;
	struct os_task task_hdl;
	uint32_t s_offset;
	uint32_t d_offset;
	uint32_t res_len;
	uint32_t buf_len;
	uint32_t buf_index;
	int8_t status;
	int16_t *cache_buf;
	int16_t *buf[4];
	struct data_structure *current_data;
};
static struct audio_dac_priv *audac_priv = NULL;

typedef struct audio_da_config
{
	struct audac_device *dac;
	void 	*current_node;
	void 	*reg_node;
    audio_da_write irq_func;
	void 	*play_empty_buf;	//作为喇叭的时候,需要配置,size与buf_size一致,可以是malloc也可以是固定,如果是malloc,需要自己去free
	int 	 buf_size;	
	uint8_t audio_hz;
	uint8_t is_empty;
}audio_da_config;

audio_da_config global_audio_da;

int get_audio_dac_set_filter_type(void);
void audio_dac_set_filter_type(int filter_type);
int audio_dac_get_samplingrate(void);

void audio_dac_irq(uint32 irq, uint32 irq_data)
{
	audio_da_config *audio_da_cfg = (audio_da_config *)irq_data;

    //满
    if (irq & AUDAC_IRQ_FLAG_FULL) {
		if(audio_da_cfg->current_node) {
			audio_da_cfg->current_node = NULL;
			os_sema_up(&audac_priv->cache_sema);
		}
		else {
			audio_da_cfg->is_empty = 1;
		}
		if(audio_da_cfg->reg_node) {
			audio_da_cfg->current_node = audio_da_cfg->reg_node;
			audio_da_cfg->reg_node = NULL;
		}
    }

    //半
    if (irq & AUDAC_IRQ_FLAG_HALF) { 
		if(audio_da_cfg->reg_node) {
			os_sema_up(&audac_priv->cache_sema);
		}
		if(audac_priv->status > 0) {
			audac_priv->status--;
			audio_da_cfg->reg_node = audac_priv->buf[audac_priv->buf_index%4];
			audac_priv->buf_index++;
			audio_da_cfg->irq_func(audio_da_cfg , audio_da_cfg->reg_node, audac_priv->buf_len);
			audio_da_cfg->is_empty = 0;
		}
		else {
			if(audio_da_cfg->play_empty_buf)
			{
				audio_da_cfg->irq_func(audio_da_cfg , audio_da_cfg->play_empty_buf, audio_da_cfg->buf_size);
			}
			extern uint32 del_node_num;
			extern uint32 g_intercom_start;
			if(g_intercom_start)
			{
				del_node_num++;
			}
		}
    }
}


static int32 global_audio_da_write(struct audio_da_config *audio, void* buf, uint32 len)
{
#if AEC_PROCESS == 1
	push_farbuf(buf, len/2, 8000);
#endif
	audac_write(audio->dac, buf, len);
	return 0;
}

void audac_priv_clear()
{
	audac_priv->res_len = CACHE_BUF_LEN;
	audac_priv->d_offset = 0;
	audac_priv->s_offset = 0;
}

void audio_dac_cache(void *d)
{
	stream *s = (stream *)d;
	struct data_structure  *data = NULL;
	int16_t *s_buf = NULL;
	uint32_t get_buf_len = 0;
	uint8_t buf_index = 0;
	static uint8_t re_play = 0;

	audac_priv_clear();
	audac_priv->status = 0;
	audac_priv->buf_index = 0;

	while(1) {
		data = recv_real_data(s);
		if(data)
		{
			audac_priv->current_data = data;
			s_buf = get_stream_real_data(data);
			get_buf_len = get_stream_real_data_len(data);
			audac_priv->s_offset = 0;		
			while(get_buf_len >= audac_priv->res_len) {
				hw_memcpy(audac_priv->buf[buf_index%4]+(audac_priv->d_offset/2), s_buf+(audac_priv->s_offset/2), audac_priv->res_len);
				get_buf_len -= audac_priv->res_len;
				audac_priv->buf_len = CACHE_BUF_LEN;
				audac_priv->s_offset += audac_priv->res_len;
				audac_priv->cache_buf = audac_priv->buf[buf_index%4];
				hw_memcpy(copy_buf, audac_priv->cache_buf, audac_priv->buf_len);
				copy_len = audac_priv->buf_len;
				audac_priv->status++;		
				re_play = 0;
				os_sema_down(&audac_priv->cache_sema, -1);
				audac_priv->res_len = CACHE_BUF_LEN;
				audac_priv->d_offset = 0;
				buf_index++;
			}
			if(get_buf_len) {
				hw_memcpy(audac_priv->buf[buf_index%4]+(audac_priv->d_offset/2), s_buf+(audac_priv->s_offset/2), get_buf_len);
				audac_priv->res_len = CACHE_BUF_LEN - get_buf_len;
				audac_priv->d_offset = get_buf_len;
				audac_priv->s_offset = 0;
				get_buf_len = 0;
			}
			free_data(data);
			audac_priv->current_data = NULL;
		}
		else {
				os_sleep_ms(1);
		}
	}
}

static int opcode_func(stream *s,void *priv,int opcode)
{
	int res = 0;
	switch(opcode)
	{
		case STREAM_OPEN_ENTER:
		break;
		case STREAM_OPEN_EXIT:
		{
			enable_stream(s,1);
			s->priv = (void*)SOUND_ALL;
			audac_priv = (struct audio_dac_priv*)ADUIO_ZALLOC(sizeof(struct audio_dac_priv));
			if(audac_priv) {
				audac_priv->buf[0] = (int16_t *)custom_malloc(CACHE_BUF_LEN);
				audac_priv->buf[1] = (int16_t *)custom_malloc(CACHE_BUF_LEN);
				audac_priv->buf[2] = (int16_t *)custom_malloc(CACHE_BUF_LEN);
				audac_priv->buf[3] = (int16_t *)custom_malloc(CACHE_BUF_LEN);
				os_sema_init(&audac_priv->cache_sema,3);
				OS_TASK_INIT("audio_dac_cache", &audac_priv->task_hdl, audio_dac_cache, s, OS_TASK_PRIORITY_ABOVE_NORMAL, 1024);
			}
		}
		break;
		case STREAM_OPEN_FAIL:
		break;
		case STREAM_RECV_DATA_FINISH:
		break;
		//在发送到这个流的时候,进行数据包过滤
		case STREAM_FILTER_DATA:
			{
				struct data_structure *data = (struct data_structure *)priv;
				int filter_type  = (int)s->priv;
				if(GET_DATA_TYPE1(data->type) != SOUND)
				{
					res = 1;
					break;
				}
				//永远不过滤
				if(!filter_type || GET_DATA_TYPE2(data->type) == SOUND_ALL)
				{
				}
				else
				{
					//过滤不匹配的数据包
					if(!(filter_type && (filter_type == GET_DATA_TYPE2(data->type))))
					{
						//os_printf("filter_type:%d\tdata_type:%d\t%X\n",filter_type,GET_DATA_TYPE2(data->type),s);
						res = 1;
						break;
					}
				}

			}
		break;

		//流接收后,数据包也要检查是否需要过滤或者是不是因为逻辑条件符合需要过滤
		case STREAM_RECV_FILTER_DATA:
			{
				struct data_structure *data = (struct data_structure *)priv;
				if(GET_DATA_TYPE1(data->type) != SOUND)
				{
					res = 1;
					break;
				}

				
				int filter_type  = (int)s->priv;
				//永远不过滤
				if(!filter_type ||  GET_DATA_TYPE2(data->type) == SOUND_ALL)
				{
				}
				else
				{
					//过滤不匹配的数据包
					if(!(filter_type && (GET_DATA_TYPE2(filter_type)== GET_DATA_TYPE2(data->type))))
					{
						res = 1;
					}
				}
			}
		break;


		//接收到命令,可以尝试执行命令的接口
		case STREAM_SEND_CMD:
		{
			uint32_t cmd = (uint32_t)priv;
			//只是接受支持的命令
			if(GET_CMD_TYPE1(cmd) == CMD_AUDIO_DAC)
			{
				os_printf("!!!!!!!!!cmd:%X\n",cmd);
				s->priv = (void*)GET_CMD_TYPE2(cmd);
			}
			else if(GET_CMD_TYPE1(cmd) == CMD_AUDIO_DAC_MODIFY_HZ)
			{
				extern void audio_da_recfg(uint32_t hz);
				audio_da_recfg(GET_CMD_TYPE2(cmd));
			}
			
		}
		break;


		default:
		break;
	}
	return res;
}


//优先创建音频的流
stream *audio_dac_stream_init(const char *name)
{
	stream *s = open_stream_available(name,0,AUDIONUM,opcode_func,NULL);
	if(s)
	{
		global_audio_dac_s = s;
	}
	return s;
}

//关闭音频流
void audio_dac_stream_deinit()
{
	int res;
	if(global_audio_dac_s)
	{
		res = close_stream(global_audio_dac_s);
		if(!res)
		{
			global_audio_dac_s = NULL;
		}
	}
}




void audio_da_init()
{

	struct aufade_device *fade = (struct aufade_device *)dev_get(HG_AUFADE_DEVID);
	aufade_open(fade);
	os_printf("%s:%d\n",__FUNCTION__,__LINE__);

    struct audac_device *audio_da = (struct audac_device *)dev_get(HG_AUDAC_DEVID);

    memset(&global_audio_da,0,sizeof(global_audio_da));
	audio_da_config *audio_da_cfg = &global_audio_da;
	audio_da_cfg->dac = audio_da;

	audio_da_cfg->buf_size = AUDIOLEN;
	audio_da_cfg->play_empty_buf = empty_buf;
    audio_da_cfg->irq_func = global_audio_da_write;
	audio_da_cfg->audio_hz = AUDAC_SAMPLE_RATE_8K;
	
	stream *dest = audio_dac_stream_init(R_SPEAKER);

    audac_open(audio_da, audio_da_cfg->audio_hz );
	os_printf("audio_da_cfg:%X\n",audio_da_cfg);
    audac_request_irq(audio_da, AUDAC_IRQ_FLAG_HALF | AUDAC_IRQ_FLAG_FULL, (audac_irq_hdl)audio_dac_irq, (uint32_t)audio_da_cfg);
    audio_da_cfg->irq_func(audio_da_cfg , audio_da_cfg->play_empty_buf, audio_da_cfg->buf_size);
	global_audio_dac_s = dest;

    return;
}

void audio_da_deinit()
{
	struct audac_device *audio_da = (struct audac_device *)dev_get(HG_AUDAC_DEVID);
	struct aufade_device *fade = (struct aufade_device *)dev_get(HG_AUFADE_DEVID);
	audio_da_config *audio_da_cfg = &global_audio_da;
	aufade_close(fade);
	audac_close(audio_da);
	prev_filter_type = get_audio_dac_set_filter_type();
	audio_dac_set_filter_type(SOUND_NONE);
	//清除中断没有处理完的数据
	if(audac_priv->current_data)
	{
		free_data(audac_priv->current_data);
		audac_priv->current_data = NULL;
	}
	if(audio_da_cfg->reg_node) {
		os_sema_up(&audac_priv->cache_sema);
	}
	if(audio_da_cfg->current_node) {
		os_sema_up(&audac_priv->cache_sema);
	}
}

void audio_da_reinit()
{
	struct audac_device *audio_da = (struct audac_device *)dev_get(HG_AUDAC_DEVID);
	struct aufade_device *fade = (struct aufade_device *)dev_get(HG_AUFADE_DEVID);
	aufade_open(fade);
	//这里成立的前提是原来stream已经创建过,否则可能有问题
	stream *dest = audio_dac_stream_init(R_SPEAKER);
    memset(&global_audio_da,0,sizeof(global_audio_da));
	audio_da_config *audio_da_cfg = &global_audio_da;
	audio_da_cfg->dac = audio_da;

	audio_da_cfg->buf_size = AUDIOLEN;
	audio_da_cfg->play_empty_buf = empty_buf;
    audio_da_cfg->irq_func = global_audio_da_write;
	audio_da_cfg->audio_hz = AUDAC_SAMPLE_RATE_8K;

    audac_open(audio_da, audio_da_cfg->audio_hz);
	os_printf("!!!!audio_da_cfg:%X\n",audio_da_cfg);
    audac_request_irq(audio_da, AUDAC_IRQ_FLAG_HALF | AUDAC_IRQ_FLAG_FULL, (audac_irq_hdl)audio_dac_irq, (uint32_t)audio_da_cfg);
    audio_da_cfg->irq_func(audio_da_cfg , audio_da_cfg->play_empty_buf, audio_da_cfg->buf_size);
	global_audio_dac_s = dest;
	//这个函数只是重新初始化dac硬件,所以流与init的时候有区别,这里需要关闭一次(实际内部没有关闭,与audio_dac_stream_init成对使用)
	audio_dac_stream_deinit();
	audio_dac_set_filter_type(prev_filter_type);
	
}


//音频采样率重新修改
void audio_da_recfg(uint32_t hz)
{
	os_printf("%s hz:%d\n",__FUNCTION__,hz);
	//识别采样率,如果一样,则直接退出
	int8_t now_hz_enum = -1;
	switch(hz)
	{
		case 8000:
			now_hz_enum = AUDAC_SAMPLE_RATE_8K;
		break;
		case 11025:
			now_hz_enum = AUDAC_SAMPLE_RATE_11_025K;
		break;
		case 16000:
			now_hz_enum = AUDAC_SAMPLE_RATE_16K;
		break;
		case 22050:
			now_hz_enum = AUDAC_SAMPLE_RATE_22_05K;
		break;
		case 32000:
			now_hz_enum = AUDAC_SAMPLE_RATE_32K;
		break;
		case 44100:
			now_hz_enum = AUDAC_SAMPLE_RATE_44_1K;
		break;
		case 48000:
			now_hz_enum = AUDAC_SAMPLE_RATE_48K;
		break;
		case 24000:
			now_hz_enum = AUDAC_SAMPLE_RATE_24K;
		break;
		case 12000:
			now_hz_enum = AUDAC_SAMPLE_RATE_12K;
		break;
		default:
			now_hz_enum = -1;
		break;
	}

	audio_da_config *audio_da_cfg = &global_audio_da;
	os_printf("now_hz_enum:%d\tlast now_hz_enum:%d\n",now_hz_enum,audio_da_cfg->audio_hz);

	//采样率不需要修改或者采样率设置错误
	if(audio_da_cfg->audio_hz == now_hz_enum || now_hz_enum == -1)
	{
		return;
	}

	struct audac_device *audac_dev = (struct audac_device *)dev_get(HG_AUDAC_DEVID);

	//将当前流的数据也删除,然后才可以接收其他正确采样率的数据
	int last_type = get_audio_dac_set_filter_type();

	audio_dac_set_filter_type(SOUND_NONE);
	//等待播放数据完毕,然后切换采样率
	uint32_t count = 0;
	//等待中断数据播放完毕
	while( !(audio_da_cfg->is_empty) && count++<1000)
	{
		os_sleep_ms(1);
	}

	audac_ioctl(audac_dev,AUDAC_IOCTL_CMD_CHANGE_SAMPLE_RATE,now_hz_enum,0);
	audio_dac_set_filter_type(last_type);


	audio_da_cfg->audio_hz = now_hz_enum;
	os_printf("audio_da_cfg:%X\tnow_hz_enum:%d\n",audio_da_cfg,now_hz_enum);
    audio_da_cfg->irq_func(audio_da_cfg , audio_da_cfg->play_empty_buf, audio_da_cfg->buf_size);
}



//写一些通用dac输出的接口
/***********************************************************
设置接收和播放声音的类型(只是将当前不匹配的音频都会过滤掉)
使用场景:比如进入音乐播放后,就不再响应按键的音频了
***********************************************************/
void audio_dac_set_filter_type(int filter_type)
{
	if(global_audio_dac_s)
	{
		os_printf("filter_type:%d\t%X\n",filter_type,global_audio_dac_s);
		global_audio_dac_s->priv = (void*)filter_type;
	}
}


void print_audio_dac_set_filter_type()
{
	if(global_audio_dac_s)
	{
		os_printf("type:%X\n",global_audio_dac_s->priv);
	}
}

int get_audio_dac_set_filter_type(void)
{
	if(global_audio_dac_s)
	{
		return (int)global_audio_dac_s->priv;
	}
	return 0;
}

int audio_dac_get_samplingrate(void)
{
	audio_da_config *play = &global_audio_da;
	int32_t samplingrate = -1;
	
	switch(play->audio_hz)
	{
		case AUDAC_SAMPLE_RATE_8K:
			samplingrate = 8000;
			break;
		case AUDAC_SAMPLE_RATE_11_025K:
			samplingrate = 11025;
			break;
		case AUDAC_SAMPLE_RATE_12K:
			samplingrate = 12000;
			break;
		case AUDAC_SAMPLE_RATE_16K:
			samplingrate = 16000;
			break;
		case AUDAC_SAMPLE_RATE_22_05K:
			samplingrate = 22050;
			break;
		case AUDAC_SAMPLE_RATE_24K:
			samplingrate = 24000;
			break;
		case AUDAC_SAMPLE_RATE_32K:
			samplingrate = 32000;
			break;
		case AUDAC_SAMPLE_RATE_44_1K:
			samplingrate = 44100;
			break;
		case AUDAC_SAMPLE_RATE_48K:
			samplingrate = 48000;
			break;
			
	}
	printf("now DAC samplingrate:%d\n",samplingrate);
	return samplingrate;
}
