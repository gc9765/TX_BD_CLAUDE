/**
 * @file txw81x_voice.c txw81 sound driver -- source/player
 *
 * Copyright (C) Baidu 2025
 */

#include "osal/string.h"
#include "custom_mem/custom_mem.h"
#include "stream_frame.h"
#include "sys_config.h"	
#include "tx_platform.h"
#include "list.h"
#include "dev.h"
#include "devid.h"
#include "utlist.h"
#include "jpgdef.h"
#include "osal/task.h"
#include "osal_file.h"
#include "txw81x_video.h"

// #define JPEG_DUMP

#ifdef PSRAM_HEAP
#define TX_JPEG_MALLOC custom_malloc_psram
#define TX_JPEG_FREE custom_free_psram
#else
#define TX_JPEG_MALLOC custom_malloc
#define TX_JPEG_FREE custom_free
#endif

void jpeg_capture_thread(void *d);

static int opcode_func(stream *s, void *priv, int opcode)
{
    int res = 0;
    _os_printf("%s:%d\topcode:%d\n", __FUNCTION__, __LINE__, opcode);
    switch (opcode)
    {
    case STREAM_OPEN_ENTER:
        break;
    case STREAM_OPEN_EXIT:
    {
        enable_stream(s, 1);
    }
    break;
    case STREAM_OPEN_FAIL:
        break;
    default:
        // 默认都返回成功
        break;
    }
    return res;
}

int no_frame_record_jpeg(void *d, uint8_t** jpeg_data)
{
    uint8_t *jpeg_buf_addr = NULL;
    void * get_f = (void *)d;
 	uint32_t total_len;
	struct stream_jpeg_data_s *el,*tmp;

	struct data_structure  *data = (struct data_structure  *)d;
    struct stream_jpeg_data_s *dest_list = (struct stream_jpeg_data_s *)GET_DATA_BUF(data);
    struct stream_jpeg_data_s *dest_list_tmp = dest_list;
    total_len = GET_DATA_LEN(data);

	uint32_t uint_len = GET_NODE_LEN(data);
	uint32 jpg_len = total_len;

	uint8_t* jpeg_out = (uint8_t*)custom_malloc_psram(jpg_len);
	memset(jpeg_out, 0, jpg_len);
	uint32_t offset = 0;

	int jpg_count = 0;

    LL_FOREACH_SAFE(dest_list,el,tmp)
    {
        if(dest_list_tmp == el)
        {
            continue;
        }
        //读取完毕删除
        //图片保存起来
        if(total_len)
        {
            jpeg_buf_addr = (uint8_t *)GET_JPG_SELF_BUF(data,el->data);
            if(total_len >= uint_len)
            {
				hw_memcpy(jpeg_out + offset, jpeg_buf_addr, uint_len);

				offset += uint_len;
                total_len -= uint_len;
            }
            else
            {
				hw_memcpy(jpeg_out + offset, jpeg_buf_addr, total_len);
				offset += total_len;
                total_len = 0;
            }
        }

		DEL_JPEG_NODE(get_f,el);
		jpg_count++;
    }

	*jpeg_data = jpeg_out;
	return jpg_len;
}

extern int no_frame_record_video2(void *fp, void *d, int flen);
void jpeg_capture_thread(void *d)
{
    os_printf("%s[%d] Entry ...\n", __FUNCTION__, __LINE__);
    struct data_structure *get_f = NULL;
    struct photo_st *p_s = (struct photo_st *)d;
    stream *s = NULL;
    uint32_t flen;
#ifdef SRC_DUMP
    char filename[64] = {0};
    void *fp = NULL;
#endif
    s = open_stream_available(R_AT_SAVE_PHOTO, 0, 8, opcode_func, NULL);
    if (!s)
    {
        os_printf("%s[%d] open_stream_available failed.\n", __FUNCTION__, __LINE__);
        goto jpeg_capture_thread_end;
    }
    p_s->running = 1;
    uint32_t err_count = 0;
    start_jpeg();
    while (p_s->photo_num && p_s->running)
    {
        get_f = recv_real_data(s);
        if (get_f)
        {
            err_count = 0;
#ifdef SRC_DUMP
            os_sprintf(filename, "0:tx_photo_%04d.jpg", (uint32_t)os_jiffies() % 9999);
            os_printf("filename:%s\n", filename);
            fp = osal_fopen(filename, "wb+");
            if (!fp)
            {
                goto jpeg_capture_thread_end;
            }
#endif
            flen = get_stream_real_data_len(get_f);

#ifdef SRC_DUMP            
            no_frame_record_video2(fp,get_f,flen);
#endif
            uint8_t *jpeg_data = NULL;
            uint32 jpeg_len = 0;
            jpeg_len = no_frame_record_jpeg(get_f, &jpeg_data);
            os_printf("%s[%d] record jpeg  flen =%d jpeg_len = %d jpeg_data = %08x\n", __FUNCTION__, __LINE__, flen, jpeg_len, jpeg_data);
            if (p_s->rh && jpeg_data)
            {
                p_s->rh(jpeg_data, jpeg_len, NULL);
            }

            if (jpeg_data)
            {
                custom_free_psram(jpeg_data);
            }

            free_data(get_f);
            get_f = NULL;
#ifdef SRC_DUMP     
            osal_fclose(fp);
#endif
            p_s->photo_num--;
        }
        else
        {
            err_count++;
            os_sleep_ms(1);
            if (err_count++ > 1000)
            {
                goto jpeg_capture_thread_end;
            }
        }

        // 间隔 1000ms 取一帧进行视觉理解， 间隔太短会造成资源过度消耗
        os_sleep_ms(1000);
    }

jpeg_capture_thread_end:
    os_printf("%s[%d] Entry jpeg_capture_thread_end\n", __FUNCTION__, __LINE__);
    if (get_f)
    {
        free_data(get_f);
        get_f = NULL;
    }

    if (s)
    {
        stop_jpeg();
        close_stream(s);
    }
    p_s->running = 0;
}

void txw81_jpeg_destructor(void *arg)
{
    os_printf("%s:%d entry\n", __FUNCTION__, __LINE__);
    struct photo_st *st = arg;
    st->running = 0;
    sys_usleep(30);
    os_task_del(&st->task);
    TX_JPEG_FREE(st);
}

int video_txw81_jpeg_alloc(struct photo_st **stp, jpeg_read_h *rh, void *arg)
{
    struct photo_st *photo_s;
    photo_s = TX_JPEG_MALLOC(sizeof(struct photo_st));
    os_printf("%s[%d]\n", __FUNCTION__, __LINE__);
    if (photo_s)
    {
        os_printf("%s[%d]\n", __FUNCTION__, __LINE__);
        memset(photo_s, 0, sizeof(struct photo_st));
        // 持续拍照多少张进行视觉处理， 为防止视觉过度消耗，demo侧暂定一次处理120帧（约120S）图片
        photo_s->photo_num = 120;
        photo_s->rh = rh;
        photo_s->running = 1;
        // 创建拍照的任务
        OS_TASK_INIT("brtc_visual", &photo_s->task, jpeg_capture_thread, (uint32)photo_s, OS_TASK_PRIORITY_NORMAL, 1024);
    }

    *stp = photo_s;
    return 0;
}
