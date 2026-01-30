/**
 * @file txw81x_voice.c txw81 sound driver -- source/player
 *
 * Copyright (C) Baidu 2025
 */

#include "osal/string.h"
#include "custom_mem/custom_mem.h"
#include "stream_frame.h"
#include "osal/task.h"
#include "osal_file.h"

#include "txw81x_voice.h"

#define STACK_SIZE 1024 * 3
#define MSG_SIZE 256

#define AUDIO_TUNING
// #define SRC_DUMP

static int kill_src_req = 0;
static int kill_play_req = 0;

extern int get_audio_dac_set_filter_type(void);
extern void audio_dac_set_filter_type(int filter_type);
#define AUDIO_LEN (1024)

#ifdef PSRAM_HEAP
#define TX_VOICE_MALLOC custom_malloc_psram
#define TX_VOICE_FREE custom_free_psram
#else
#define TX_VOICE_MALLOC custom_malloc
#define TX_VOICE_FREE custom_free
#endif

static struct ausrc_st *g_txw81_srcst = NULL;
static struct auplay_st *g_txw81_playst = NULL;

static uint32_t get_sound_data_len(void *data) {
    struct data_structure *d = (struct data_structure *)data;
    return (uint32_t)d->priv;
}
static uint32_t set_sound_data_len(void *data, uint32_t len) {
    struct data_structure *d = (struct data_structure *)data;
    d->priv = (void *)len;
    return (uint32_t)len;
}
static const stream_ops_func stream_sound_ops = {
        .get_data_len = get_sound_data_len,
        .set_data_len = set_sound_data_len,
};

static int src_opcode_func(stream *s, void *priv, int opcode) {
    int res = 0;
    switch (opcode) {
    case STREAM_OPEN_ENTER:
        break;
    case STREAM_OPEN_EXIT: {
        printf("%s:%d\topcode:%d STREAM_OPEN_EXIT\n", __FUNCTION__, __LINE__, opcode);
        enable_stream(s, 1);
    } break;
    case STREAM_OPEN_FAIL:
        break;
    default:
        //默认都返回成功
        break;
    }
    return res;
}

static int play_opcode_func(stream *s, void *priv, int opcode) {
    static uint8_t *audio_buf = NULL;
    int res = 0;
    switch (opcode) {
    case STREAM_OPEN_ENTER:
        break;
    case STREAM_OPEN_EXIT: {
        printf("%s:%d\topcode:%d STREAM_OPEN_EXIT\n", __FUNCTION__, __LINE__, opcode);
        audio_buf = TX_VOICE_MALLOC(8 * AUDIO_LEN);
        if (audio_buf) {
            os_memset(audio_buf, 0, 8 * AUDIO_LEN);
            os_printf("at audio malloc:%x\n", audio_buf);
            stream_data_dis_mem(s, 8);
        }
        streamSrc_bind_streamDest(s, R_SPEAKER);
    } break;
    case STREAM_OPEN_FAIL:
        printf("%s:%d STREAM_OPEN_FAIL \n", __FUNCTION__, __LINE__);
        break;

    case STREAM_FILTER_DATA:
        break;

    case STREAM_DATA_DIS: {
        struct data_structure *data = (struct data_structure *)priv;
        if (data) {
            int data_num = (int)data->priv;
            data->ops = (stream_ops_func *)&stream_sound_ops;
            data->data = audio_buf + (data_num)*AUDIO_LEN;
        }

    } break;

    case STREAM_DATA_DESTORY: {
        if (audio_buf) {
            TX_VOICE_FREE(audio_buf);
            audio_buf = NULL;
        }
    }
    case STREAM_DATA_FREE:
        // printf("%s:%d\n",__FUNCTION__,__LINE__);
        break;

    //数据发送完成,可以选择唤醒对应的任务
    case STREAM_RECV_DATA_FINISH:
        break;

    default:
        //默认都返回成功
        break;
    }
    return res;
}

static void voip_src_task(void *priv) {
    struct ausrc_st *st = priv;
    if (!st || !st->ready) {
        printf("%s start audio src task failed.\r\n", __func__);
        return;
    }

    uint32_t count = 0;
    uint8_t *dev_buf = NULL;
    uint32_t dev_buf_len = 0;
    struct data_structure *src_data = NULL;
    stream *src_stream = NULL;

#ifdef SRC_DUMP
    int w_len = 0;
    int w_count = 0;
    int total_size = 0;
    char filename[64] = {0};
    void *fp  = NULL;
    uint32_t start_time = 0;
#endif

    os_printf("Entry %s:%d\n", __FUNCTION__, __LINE__);
    src_stream = open_stream_available(R_SPEECH_RECOGNITION, 0, 8, src_opcode_func, NULL);
    if (!src_stream) {
        goto audio_src_thread_end;
    }

#ifdef SRC_DUMP
    os_memcpy(st->filename_prefix, "def", 3);
    os_printf("prefix:%s\n", st->filename_prefix);
    os_sprintf(filename, "0:def_%04d.pcm", (uint32_t)os_jiffies() % 9999);
    os_printf("record name:%s\n", filename);
    fp = osal_fopen(filename,"wb+");
    if(!fp)
    {
        os_printf("Entry %s:%d\n", __FUNCTION__, __LINE__);
        goto audio_src_thread_end;
    }
    start_time = os_jiffies();
#endif

    while (1) {
        os_sleep_ms(10);

#ifdef SRC_DUMP
        if ((os_jiffies() - start_time) / 1000 > 2 * 60)
        {
            os_printf("Entry %s:%d\n", __FUNCTION__, __LINE__);
            break;
        }
#endif

        int src_data_time = os_jiffies();
        src_data = recv_real_data(src_stream);

        if (src_data) {
            dev_buf = get_stream_real_data(src_data);
            dev_buf_len = get_stream_real_data_len(src_data);

#ifdef SRC_DUMP
            w_len = osal_fwrite(dev_buf, dev_buf_len, 1, fp);
            total_size += dev_buf_len;
            w_count ++;
#endif

#ifdef AUDIO_TUNING
            if (count++ % 100 == 0 && dev_buf_len > 16) {
                os_printf(
                        "%s:%d dev_buf_len %d rec audio data: %x %x %x %x %x %x %x %x %x %x %x %x "
                        "%x %x %x %x\n",
                        __FUNCTION__,
                        __LINE__,
                        dev_buf_len,
                        dev_buf[0],
                        dev_buf[1],
                        dev_buf[2],
                        dev_buf[3],
                        dev_buf[4],
                        dev_buf[5],
                        dev_buf[6],
                        dev_buf[7],
                        dev_buf[8],
                        dev_buf[9],
                        dev_buf[10],
                        dev_buf[11],
                        dev_buf[12],
                        dev_buf[13],
                        dev_buf[14],
                        dev_buf[15]);
            }
#endif
            (void)aubuf_write(st->aubuf, dev_buf, dev_buf_len);

            free_data(src_data);
            src_data = NULL;
        }

        size_t len = st->sampc * 2;
        if (st->rh && aubuf_cur_size(st->aubuf) >= len) {
            aubuf_read(st->aubuf, st->sampv, len);
            st->rh(st->sampv, len, st->arg);
        }

        if (kill_src_req) {
            sys_usleep(1);
            break;
        }
    }

audio_src_thread_end:
    os_printf("%s audio_src_thread_end ... \n", __FUNCTION__);
#ifdef SRC_DUMP
    os_printf("%s audio_src_thread_end total_size %d frame count %d  ... \n", __FUNCTION__, total_size, w_count);
    if(fp)
    {
        osal_fclose(fp);
    }
#endif

    if (src_stream) {
        os_sleep_ms(10);
        enable_stream(src_stream, 0);
        os_sleep_ms(10);
        close_stream(src_stream);
    }

    return;
}

void ausrc_write_handler(void *sampv, size_t sampc, void *arg) {
    struct auplay_st *st = arg;
    if (!st || !st->ready) {
        printf("%s start audio src task failed.\r\n", __func__);
        return;
    }

    (void)aubuf_write(st->aubuf, sampv, sampc);
}

static void voip_play_task(void *priv) {
    struct auplay_st *st = priv;
    volatile uint8_t *pcm_buff = NULL;
    struct data_structure *play_data = NULL;
    uint32_t count = 0;
    int ret = 0;

    os_printf("Entry %s:%d\n", __FUNCTION__, __LINE__);
    stream *remote_stream = open_stream_available("brtc_playout", 8, 0, play_opcode_func, NULL);
    if (!remote_stream) {
        os_printf("\nopen stream err");
        return;
    }
    enable_stream(remote_stream, 1);

    while (1) {
        os_sleep_ms(20);

        // FIXME： 需指定len 为AUDIO_LEN，否则音频播出有沙沙异响， 平台限制
        size_t len = AUDIO_LEN;
        size_t available = aubuf_cur_size(st->aubuf);
        if (available >= len) {
            int src_data_time = os_jiffies();
            play_data = get_src_data_f(remote_stream);

            if (play_data) {
                pcm_buff = (uint8_t *)get_stream_real_data(play_data);

                aubuf_read(st->aubuf, pcm_buff, len);

#ifdef AUDIO_TUNING
                uint8_t *dev_pcm = (uint8_t *)pcm_buff;
                if (count++ % 100 == 0 && st->sampc > 16) {
                    os_printf(
                            "%s:%d %p %d sampc %d \t play audio data: %x %x %x %x %x %x %x %x %x "
                            "%x %x %x %x %x %x %x \n",
                            __FUNCTION__,
                            __LINE__,
                            pcm_buff,
                            sizeof(dev_pcm[0]),
                            st->sampc,
                            dev_pcm[0],
                            dev_pcm[1],
                            dev_pcm[2],
                            dev_pcm[3],
                            dev_pcm[4],
                            dev_pcm[5],
                            dev_pcm[6],
                            dev_pcm[7],
                            dev_pcm[8],
                            dev_pcm[9],
                            dev_pcm[10],
                            dev_pcm[11],
                            dev_pcm[12],
                            dev_pcm[13],
                            dev_pcm[14],
                            dev_pcm[15]);
                }
#endif

                play_data->type = SET_DATA_TYPE(SOUND, SOUND_MIC);
                set_sound_data_len(play_data, len);
                send_data_to_stream(play_data);
            } else {
                printf("%s:%d rx audio dev underrun\n", __FUNCTION__, __LINE__);
            }
        }

        if (kill_play_req) {
            sys_usleep(1);
            break;
        }
    }

    os_printf("voip_play_task exit ...\n");
    if (remote_stream != NULL) {
        os_sleep_ms(100);
        enable_stream(remote_stream, 0);
        os_sleep_ms(100);
        close_stream(remote_stream);
        remote_stream = NULL;
    }
}

void txw81_src_destructor(void *arg) {
    os_printf("%s:%d entry\n", __FUNCTION__, __LINE__);
    struct ausrc_st *st = arg;
    st->ready = false;
    kill_src_req = 1;
    sys_usleep(30);
    os_task_del(&st->task);
    TX_VOICE_FREE(st->sampv);
    mem_deref(st->aubuf);
    st->rh = NULL;
    g_txw81_srcst = NULL;
}

void txw81_play_destructor(void *arg) {
    os_printf("%s:%d entry\n", __FUNCTION__, __LINE__);
    struct auplay_st *st = arg;
    st->ready = false;
    kill_play_req = 1;
    sys_usleep(30);
    os_task_del(&st->at_play_audio_task);
    TX_VOICE_FREE(st->sampv);
    mem_deref(st->aubuf);
    st->wh = NULL;
    g_txw81_playst = NULL;
}

int voice_txw81_src_alloc(
        struct ausrc_st **stp,
        struct ausrc_prm *prm,
        ausrc_read_h *rh,
        void *arg) {
    struct ausrc_st *st;
    int err = 0;
    size_t psize = 0;

    if (!stp || !prm) {
        printf("%s, invalid input parameters\n", __func__);
        return EINVAL;
    }

    st = TX_VOICE_MALLOC(sizeof(*st));
    if (!st) {
        printf("%s, memory allocation for ausrc_st failed\n", __func__);
        return ENOMEM;
    }

    st->rh = rh;
    st->arg = arg;
    st->ready = true;

    st->sampsz = sizeof(int16_t);
    st->sampc = prm->srate * prm->ch * prm->ptime / 1000;
    psize = st->sampsz * st->sampc;
    printf("%s, size:%d, sampc:%d, sample rate:%d, ptime:%d\n",
           __func__,
           psize,
           st->sampc,
           prm->srate,
           prm->ptime);

    g_txw81_srcst = st;
    g_txw81_srcst->srate = prm->srate;

    st->sampv = TX_VOICE_MALLOC(psize);
    if (!st->sampv) {
        printf("%s, st->sampv malloc failed, err:%d\n", __func__, ENOMEM);
        TX_VOICE_FREE(st);
        return ENOMEM;
    }

    err = aubuf_alloc(&st->aubuf, psize, psize * 25);
    if (err) {
        printf("%s, st->aubuf malloc failed, err:%d\n", __func__, ENOMEM);
        TX_VOICE_FREE(st);
        return ENOMEM;
    }

    kill_src_req = 0;

    //创建录音频的任务
    OS_TASK_INIT(
            "ext_voip_src", &st->task, voip_src_task, (uint32)st, OS_TASK_PRIORITY_ABOVE_NORMAL, 1024 * 3);

    *stp = st;

    return 0;
}

int voice_txw81_play_alloc(
        struct auplay_st **stp,
        struct ausrc_prm *prm,
        auplay_write_h *wh,
        void *arg) {
    struct auplay_st *st;
    int err = 0;
    size_t psize = 0;

    if (!stp || !prm) {
        printf("%s, invalid input parameters\n", __func__);
        return EINVAL;
    }

    st = TX_VOICE_MALLOC(sizeof(*st));
    if (!st) {
        printf("%s, memory allocation for auplay_st failed\n", __func__);
        return ENOMEM;
    }

    st->wh = wh;
    st->arg = arg;
    st->ready = true;

    g_txw81_playst = st;
    g_txw81_playst->srate = prm->srate;

    st->sampsz = sizeof(int16_t);
    st->sampc = prm->srate * prm->ch * prm->ptime / 1000;
    psize = st->sampsz * st->sampc;
    printf("%s, size:%d, sampc:%d, sample rate:%d, ptime:%d\n",
           __func__,
           psize,
           st->sampc,
           prm->srate,
           prm->ptime);

    st->sampv = TX_VOICE_MALLOC(psize);
    if (!st->sampv) {
        printf("%s, st->sampv malloc failed, err:%d\n", __func__, ENOMEM);
        TX_VOICE_FREE(st);
        return ENOMEM;
    }

    err = aubuf_alloc(&st->aubuf, psize, psize * 4);
    if (err) {
        printf("%s, st->aubuf malloc failed, err:%d\n", __func__, ENOMEM);
        TX_VOICE_FREE(st);
        return ENOMEM;
    }

    kill_play_req = 0;
    OS_TASK_INIT(
            "ext_voip_play",
            &st->at_play_audio_task,
            voip_play_task,
            (void *)st,
            OS_TASK_PRIORITY_ABOVE_NORMAL,
            3 * 1024);

    *stp = st;
    return 0;
}
