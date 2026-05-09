#include "osal/string.h"
#include "custom_mem/custom_mem.h"
#include "stream_frame.h"

#include "play_pcmtone.h"
#include "stream_define.h"
#include "osal/task.h"
#include "osal_file.h"

#define AUDIO_LEN  (480)
#define AUDIO_COUNT 4


uint32 audio_data_len;
struct os_task play_tone_task;

/* 播放状态标志：防止快速按键导致任务重叠创建 */
static volatile uint8_t s_tone_playing = 0;


static uint32_t get_sound_data_len(void *data)
{
	return AUDIO_LEN;
}
static stream_ops_func stream_sound_ops = 
{
	.get_data_len = get_sound_data_len,
};

int play_tone_thread(void *d);

// at cmd param input
int32 play_pcmtone(pcmtone_struct * tone)
{

	os_printf("## demon_play_pcmtone, s_tone_playing=%d\r\n", s_tone_playing);

	/* 检查是否已有音效在播放，防止任务重叠 */
	if (s_tone_playing) {
		os_printf("[tone] Warning: tone playing, skip new request\r\n");
		return -1;
	}

	if(tone->data_size>AUDIO_LEN)
	{
		audio_data_len = tone->data_size; //file length

		os_printf("## audio_data_len=%d \n",audio_data_len);

		/* 设置播放标志，防止重入 */
		s_tone_playing = 1;

		// 启一个线程 处理 筛选下来的 文件播放
		OS_TASK_INIT("play_pcmtone", &play_tone_task, play_tone_thread, (uint32)tone, OS_TASK_PRIORITY_NORMAL, 1024);
	}
	return 0;
}

static int opcode_func(stream *s,void *priv,int opcode)
{
	static uint8 *audio_buf = NULL;
	int res = 0;
	switch(opcode)
	{
		case STREAM_OPEN_ENTER:
		break;
		case STREAM_OPEN_EXIT:
		{			
			audio_buf = os_malloc(AUDIO_COUNT * AUDIO_LEN);
			if(audio_buf)
			{
				stream_data_dis_mem(s,AUDIO_COUNT);
			}
			streamSrc_bind_streamDest(s,R_SPEAKER);			
		}
		break;
		case STREAM_OPEN_FAIL:
		break;

		case STREAM_FILTER_DATA:
		break;

		case STREAM_DATA_DIS:
		{
			struct data_structure *data = (struct data_structure *)priv;
			int data_num = (int)data->priv;
			data->ops = &stream_sound_ops;
			data->data = audio_buf + (data_num)*AUDIO_LEN;
		}
		break;

		case STREAM_DATA_FREE:
			//_os_printf("%s:%d\n",__FUNCTION__,__LINE__);
		break;


		//数据发送完成,可以选择唤醒对应的任务
		case STREAM_RECV_DATA_FINISH:
		break;

		//退出,则关闭对应得流
		case STREAM_CLOSE_EXIT:
		{
			if(audio_buf)
			{
				os_free(audio_buf);
				audio_buf = NULL;
			}
		}
		break;

		default:
			//默认都返回成功
		break;
	}
	return res;	
}

uint8 *pcm_tone_fp = NULL;
int play_tone_thread(void *d)
{
	int readLen = 0;
	uint32 read_total_len = 0;
	struct data_structure *data = NULL;
	int former_dac_priv = 0;

	pcmtone_struct *fp = (pcmtone_struct*)d;

	// 开一个流 AUDIO_COUNT个发送节点（data）
	// 添加重试机制，避免系统资源未就绪时创建失败
	stream *src = NULL;
	int retry_count = 0;
	int max_retry = 5;

	while(!src && retry_count < max_retry) {
		src = open_stream_available("tone_audio", AUDIO_COUNT, 0, opcode_func, NULL);
		if(!src) {
			retry_count++;
			if(retry_count < max_retry) {
				os_sleep_ms(100);  // 等待100ms后重试
			}
		}
	}

	if(!src) // 流没开成的话也要释放 退出
	{
		os_printf("\n[tone] open stream err after %d retries, s_tone_playing cleared\r\n", max_retry);
		s_tone_playing = 0;  // 清除播放标志，允许后续重试
		return 0;
	}

	former_dac_priv = get_audio_dac_set_filter_type(); // 获取dac 过滤类型
	audio_dac_set_filter_type(SOUND_FILE);

	while(read_total_len < audio_data_len)
	{
		data = get_src_data_f(src); // file_audio流里面获取一个 空的数据节点
		if(data)
		{
			//os_printf("## read_total_len=%d \n",read_total_len);

			if((read_total_len+AUDIO_LEN)<audio_data_len)
			{
				pcm_tone_fp = get_stream_real_data(data); //获取到该数据节点的实际 数据空间地址

				os_memcpy(pcm_tone_fp,(fp->data+read_total_len),AUDIO_LEN);

				read_total_len += AUDIO_LEN;
				data->type = SET_DATA_TYPE(SOUND, SOUND_FILE); // 设置 每个节点的 数据类型
				send_data_to_stream(data); //数据节点 发到流
			}
			else
			{
				break;
			}
		}
		else{
			//os_printf("\n data full ");
		}
	}
	os_printf("\n ## play tone end");

	audio_dac_set_filter_type(former_dac_priv);
	
	os_sleep_ms(50);  

	close_stream(src);
	src = NULL;

	/* 清除播放标志，允许新的音效播放 */
	s_tone_playing = 0;

	//os_task_del(&play_tone_task);
	return 0;
}