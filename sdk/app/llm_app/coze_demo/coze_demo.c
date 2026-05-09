#include "coze_demo.h"
#include "sounds.h"
#include "cJSON.h"
#include "devid.h"
#include "osal_file.h"

#ifdef COZE_DEMO
/* -------------------------- 宏 -------------------------- */
/* Please add your server information */
#define COZE_URL        "wss://ws.coze.cn/v1/chat?bot_id="
#define COZE_BOT_ID     "xxx"
#define COZE_PAT_KEY    "xxx"
#define COZE_UPDATE_URL "https://api.coze.cn/v1/files/upload"

#ifndef UINT64_MAX
#define UINT64_MAX 0xffffffffffffffffULL
#endif
#define COZE_DEMO_DIALOGUE_TIMEOUT_MAX_CNT 5
#define COZE_EVENT_QUEUE_CNT    16
#define COZE_IDLE_TIMEOUT       20000
#define OPUS_PROMPT_FRAME_LEN   60
#define SD_CACHE_SIZE           (4*1024)

/* -------------------------- 函数声明 -------------------------- */
void *llm_malloc(uint32 size) {return os_malloc(size);}
void  llm_free(void *ptr) {os_free(ptr);}
void *llm_zalloc(size_t size) {return os_zalloc(size);}
void *llm_calloc(size_t nmemb, size_t size) {return os_calloc(nmemb, size);}
void *llm_realloc(void *ptr, size_t size) {return os_realloc(ptr, size);}
char *llm_strdup(const char *s)
{
    size_t len;
    char *d;

    if (s == NULL) {
        return NULL;
    }

    len = os_strlen(s);
    d = llm_malloc(len + 1);
    if (d == NULL) {
        return NULL;
    }
    os_memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

static int32 coze_main_event_cb(void *session, uint16 evt, uint32 param1, uint32 param2);
static void coze_main_set_state(coze_demo_state new_state);
static void coze_main_timeout_warn(void);
static void coze_main_timeout_reset(void);

/* -------------------------- 全局变量 -------------------------- */
/*
 * @brief 模型列表
*/
const struct llm_model models[] = {
    {"coze_sts", (struct llm_model_data *) &coze_sts_model},
};
/*
 * @brief llm 库全局参数
*/
struct llm_global_param global = {
    .task_reuse     = 0,
    .model_count    = 1,
    .models         = models,
};

/*
 * @brief 会话层参数
*/
struct llm_sts_sparam coze_sts_session_cfg = {
    .qmsg_tx_cnt            = 16,       //audio sample msg num
    .rx_buff_size           = 4096,     //audeo recv buff
    .cb_max_size            = 2048,
    .evt_cb                 = coze_main_event_cb,
};

/*
 * @brief 平台层参数
*/

struct coze_chat_platform_cfg coze_sts_platform_cfg = {
    .host_url               = COZE_URL,
    .pat_key                = COZE_PAT_KEY,
    .bot_id                 = COZE_BOT_ID,
    .update_url             = COZE_UPDATE_URL,
    .input_audio_format     = "pcm",
    .input_audio_codec      = "g711a",
    .output_audio_codec     = "opus",
};


/*
 * @brief 传输层参数
*/
struct llm_trans_param coze_sts_trans_cfg = {
    .buffersize         = 2048,
    .upload_buffersize  = 2048,
    .low_speed_limit    = 10,
    .low_speed_time     = 5,
    .connect_timeout    = 5,
    .blob_len           = 0,
    .blob_data          = NULL,
};

/*
 * @brief 应用全局管理结构体
*/
struct coze_demo_manage coze_mgr = {
    .key_triggered      = 0,
    .voice_triggered    = 0,
    .finish             = 0,
    .connected          = 0,
    .send_audio_cnt     = 0,
    .time_stamp         = UINT64_MAX,
};

#ifdef COZE_DEMO_URL_PLAY
static const struct ufprotocol coze_ufprotos[] = {
    {"http", &uf_curl_http},
};
#endif

struct os_task coze_main_task_awaken;
struct os_task coze_main_task_demo;

struct os_task coze_audio_play_task;

/* -------------------------- 音频代码 -------------------------- */
/*
 * @brief 播放 Opus 音频
 * @param audio 音频数据指针
 * @param audio_len 音频数据长度
 * @param frame_len 音频帧长度
*/
static void coze_audio_play_opus(const char *audio, uint32 audio_len, uint16 frame_len)
{
    int32 ret = RET_OK;
    uint32 copy_len = audio_len;
    char *write_addr = (char *)audio;
    uint8 *combined_buf = NULL;
    uint32 combined_len;

    combined_len = 2 + frame_len;
    combined_buf = (uint8 *)llm_malloc(combined_len);
    if (!combined_buf) {
        os_printf("coze_audio_play_prompt_tone no buffer\r\n");
        return;
    }
    while (copy_len > 0) {
        os_memcpy(combined_buf, &frame_len, 2);
        os_memcpy(combined_buf + 2, write_addr, frame_len);
        ret = llm_sts_play_audio(coze_mgr.sts_session, (char *)combined_buf, combined_len);
        if (ret != RET_OK) { break; }
        write_addr   += frame_len;
        copy_len     -= frame_len;
    }
    llm_free(combined_buf);
}

/*
 * @brief 播放提示音
 * @param audio 音频数据指针
 * @param audio_len 音频数据长度
 * @param frame_len 音频帧长度
*/
static void coze_audio_play_prompt_tone(const char *audio, uint32 audio_len, uint16 frame_len)
{
    audac_disable_play();
    while (!audac_wait_empty()) {
        os_sleep_ms(1);
    }
    audac_enable_play();

    coze_audio_play_opus(audio, audio_len, frame_len);

    while (!audac_wait_empty()) {
        os_sleep_ms(1);
    }
}

/*
 * @brief 释放音频数据流
 * @param s 音频数据流指针
*/
void coze_audio_free_sample_stream(stream *s)
{
    struct data_structure *data_s = NULL;
    do {
        data_s = recv_real_data(s);
        if (data_s) {
            free_data(data_s);
        }
    } while (data_s != NULL);
}

/*
 * @brief 发送音频数据到服务器
 * @param s 音频数据流指针
 * @param aualaw_dev 音频编码设备指针
 * @return 发送结果，0表示成功，其他值表示失败
*/
int32 coze_audio_send_data(stream *s, struct aualaw_device *aualaw_dev)
{
    int32 ret = RET_OK;
    static uint32 timeout = 0;

    struct data_structure *data_s = NULL;
    char *data = NULL;
    uint32 data_len = 0;

    data_s = recv_real_data(s);
    if (data_s) {
        data = get_stream_real_data(data_s);
        data_len = get_stream_real_data_len(data_s);
        if (!coze_mgr.aualaw_buf)
        { coze_mgr.aualaw_buf = (char *)llm_malloc(data_len / 2); }
#ifdef PSRAM_HEAP
        if (!coze_mgr.psram_copy_buff) {
            coze_mgr.psram_copy_buff = llm_malloc(data_len);
        }
        if (coze_mgr.psram_copy_buff) {
            os_memcpy(coze_mgr.psram_copy_buff, data, data_len);
            ret = aualaw_encode(aualaw_dev, coze_mgr.psram_copy_buff, data_len, coze_mgr.aualaw_buf);
        }
#else
        ret = aualaw_encode(aualaw_dev, data, data_len, coze_mgr.aualaw_buf);
#endif
        if (ret != 0) {
            os_printf("aualaw encode err!\n");
            free_data(data_s);
            data_s = NULL;
            return RET_ERR;
        }

        if (data && coze_mgr.aualaw_buf) {
            ret = llm_sts_send(coze_mgr.sts_session, LLM_DATA_TYPE_AUDIO, LLM_DATA_STATE_MIDDLE, coze_mgr.aualaw_buf, data_len / 2);
            //os_printf("send audio len:%d\r\n", data_len / 2);
            if (ret == LLME_AGAIN) {
                timeout++;
                if (timeout == 100) {
                    coze_main_timeout_warn();
                }
                if (timeout > 500) {
                    timeout = 0;
                    coze_main_timeout_reset();
                }
            } else if (ret == RET_OK) {
                timeout = 0;
                coze_mgr.send_audio_cnt++;
            } else {
                timeout = 0;
                os_printf("send audio err!\n");
                coze_main_set_state(COZE_DEMO_STATE_IDLE);
                llm_sts_reconnect(coze_mgr.sts_session);
            }
        }
        free_data(data_s);
        data_s = NULL;
    }
    return ret;
}

/*
 * @brief 处理音频流操作码
 * @param s 音频流指针
 * @param priv 私有数据指针
 * @param opcode 操作码
 * @return 操作结果，0表示成功，其他值表示失败
*/
static int32 coze_audio_opcode_func(stream *s, void *priv, int opcode)
{
    int32 res = 0;
    switch (opcode) {
        case STREAM_OPEN_ENTER:
            break;
        case STREAM_OPEN_EXIT: {
            enable_stream(s, 1);
        }
        break;
        case STREAM_OPEN_FAIL:
            break;
        default:
            break;
    }
    return res;
}

/*
 * @brief 获取音频数据长度
 * @param data 音频数据指针
 * @return 音频数据长度
*/
static uint32_t coze_audio_get_sound_data_len(void *data)
{
    struct data_structure  *d = (struct data_structure *)data;
    return (uint32_t)d->priv;
}

/*
 * @brief 设置音频数据长度
 * @param data 音频数据指针
 * @param len 音频数据长度
 * @return 音频数据长度
*/
static uint32_t coze_audio_set_sound_data_len(void *data, uint32_t len)
{
    struct data_structure  *d = (struct data_structure *)data;
    d->priv = (void *)len;
    return (uint32_t)len;
}

static const stream_ops_func stream_sound_ops = {
    .get_data_len = coze_audio_get_sound_data_len,
    .set_data_len = coze_audio_set_sound_data_len,
};

/*
 * @brief 处理音频播放操作码
 * @param s 音频流指针
 * @param priv 私有数据指针
 * @param opcode 操作码
 * @return 操作结果，0表示成功，其他值表示失败
*/
static int coze_audio_play_opcode_func(stream *s, void *priv, int opcode)
{
    static uint8_t *audio_buf = NULL;
    int res = 0;
    switch (opcode) {
        case STREAM_OPEN_EXIT: {
            audio_buf = llm_malloc(3 * 60 * 8 * 2);  //帧长 * 每毫秒采样率 * 两个byte
            if (audio_buf) {
                stream_data_dis_mem(s, 3);
            }
            streamSrc_bind_streamDest(s, R_SPEAKER);
        }
        break;
        case STREAM_DATA_DIS: {
            struct data_structure *data = (struct data_structure *)priv;
            int data_num = (int)data->priv;
            data->ops = (stream_ops_func *)&stream_sound_ops;
            data->data = audio_buf + (data_num) * 60 * 16;
        }
        break;
        case STREAM_DATA_DESTORY: {
            if (audio_buf) {
                llm_free(audio_buf);
                audio_buf = NULL;
            }
        }
        case STREAM_DATA_FREE:
            //_os_printf("%s:%d\n",__FUNCTION__,__LINE__);
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

/*
 * @brief 音频播放线程函数
 * @param arg 线程参数指针
 * @return 无
*/
void coze_audio_play_thread(void *arg)
{
    struct data_structure *data_s = NULL;
    stream *s = NULL;
    OpusDecoder *decoder = NULL;
    uint8_t *opus_buffer = NULL;
    uint8_t *opus_buffer_temp = NULL;
    uint16_t encode_len = 0;
    uint16_t encode_len_old = 0;
    uint32_t dec_len = 0;
    int read_len = 0;

    s = open_stream_available("file_audio", 3, 0, coze_audio_play_opcode_func, NULL);
    audio_dac_set_filter_type(SOUND_FILE);

    int err = 0;;
    if (!(decoder = opus_decoder_create(8000, 1, &err))) {
        os_printf("Opus decoder init failed: %d\n", err);
        goto cleanup;
    }

    while (1) {
        os_sleep_ms(1);

        /* 数据获取阶段 */
        data_s = get_src_data_f(s);
        if (!data_s) {
            os_sleep_ms(1);
            continue;
        }
        void *pcm_data = get_stream_real_data(data_s);
        if (!pcm_data) {
            os_printf("Invalid pcm data buffer\n");
            goto data_cleanup;
        }

        /* 协议头解析阶段 */
        if ((read_len = llm_sts_recv(coze_mgr.sts_session, (char *)&encode_len, sizeof(encode_len))) != sizeof(encode_len)) {
            goto data_cleanup;
        }

        /* 数据有效性检查 */
        if (encode_len <= 0) {
            goto data_cleanup;
        }

        /* 缓冲区动态管理 */
        if (encode_len > encode_len_old) {
            opus_buffer_temp = llm_realloc(opus_buffer, encode_len);
            if (!opus_buffer_temp) {
                os_printf("Buffer allocation failed for %d bytes\n", encode_len);
                goto data_cleanup;
            }
            opus_buffer = opus_buffer_temp;
            encode_len_old = encode_len;
        }

        /* 数据接收阶段 */
        if ((read_len = llm_sts_recv(coze_mgr.sts_session, (char *)opus_buffer, encode_len)) != encode_len) {
            os_printf("Data incomplete: %d/%d\n", read_len, encode_len);
            goto data_cleanup;
        }

        /* 音频解码阶段 */
        if ((dec_len = opus_decode(decoder, opus_buffer, encode_len,
                                   pcm_data, 60 * 8, 0)) <= 0) {
            os_printf("Decode error: %d\n", dec_len);
            goto data_cleanup;
        }

        /* 数据发送阶段 */
        data_s->type = SET_DATA_TYPE(SOUND, SOUND_FILE);
        coze_audio_set_sound_data_len(data_s, 60 * 8 * 2);
        send_data_to_stream(data_s);
        data_s = NULL;
        continue;

data_cleanup:
        force_del_data(data_s);
        data_s = NULL;
    }

cleanup:
    if (decoder) {
        opus_decoder_destroy(decoder);
    }
    if (opus_buffer) {
        llm_free(opus_buffer);
    }
}

/* -------------------------- 主要代码 -------------------------- */
#ifdef LLM_SPV12XX
#define DEBOUNCE_COUNT  3       // 去抖动计数器阈值，连续检测到相同状态3次认为有效
#define COOLDOWN_TIME   3000    // 冷却时间，单位为毫秒（3秒）
/*
 * @brief ASR 芯片唤醒识别线程(普林芯驰 SPV12x 系列)
*/
void coze_main_awaken_demo(void)
{
    // 设置 LLM_WK_IO 为输入模式，并配置上拉电阻为 100K 欧姆
    gpio_set_dir(LLM_WK_IO, GPIO_DIR_INPUT);
    gpio_set_mode(LLM_WK_IO, GPIO_PULL_UP, GPIO_PULL_LEVEL_100K);

    uint64 low_start_time = 0;      // 记录引脚拉低开始的时间（单位：毫秒）
    uint64 cooldown_end_time = 0;   // 记录冷却结束的时间（单位：毫秒）
    bool is_low = false;            // 标记是否处于低电平状态
    int debounce_counter = 0;       // 去抖动计数器
    bool last_state = true;         // 记录上一次读取的状态，默认高电平

    while (1) {
        os_sleep_ms(1);
        uint64 current_time = os_jiffies_to_msecs(os_jiffies());
        // 如果处于冷却时间内，跳过检测逻辑
        if (current_time < cooldown_end_time) {
            continue;
        }

        bool current_state = gpio_get_val(LLM_WK_IO) == 0; // 当前状态，true 表示低电平
        if (current_state == last_state) {
            // 如果当前状态与上次相同，增加计数器
            debounce_counter++;
            if (debounce_counter >= DEBOUNCE_COUNT) {
                debounce_counter = DEBOUNCE_COUNT; // 防止计数器溢出

                // 如果低电平刚刚被确认有效，则记录时间为当前时间减去 DEBOUNCE_COUNT 消耗的时间
                if (!is_low && current_state) { // 从高变低
                    is_low = true;
                    low_start_time = current_time - DEBOUNCE_COUNT; // 减去去抖动消耗的时间
                }

                // 如果已经是低电平状态，检查持续时间
                if (is_low) {
                    uint64 duration = current_time - low_start_time;
                    //os_printf(KERN_ERR"LLM_WK_IO pulled low for %ums.\r\n", duration);

                    // 判断是否在 16ms 到 20ms 范围内
                    if (duration >= 16 && duration <= 20) {
                        // 持续低电平在 16ms 到 20ms 范围内，触发唤醒逻辑
                        os_printf(KERN_ERR"Wakeup triggered!\r\n");

                        is_low = false;         // 重置低电平状态
                        debounce_counter = 0;   // 重置去抖动计数器
                        last_state = true;      // 重置上次状态

                        audac_disable_play();
                        // 设置冷却结束时间
                        cooldown_end_time = current_time + COOLDOWN_TIME;
                        coze_mgr.voice_triggered = 1;
                        continue;
                    }
                }
            }
        } else {
            debounce_counter = 0;
        }
        last_state = current_state;
    }
}
#endif
/*
 * @brief 处理按键回调函数
 * @param callback_list 按键回调列表指针
 * @param keyvalue 按键值
 * @param extern_value 外部值
 * @return 操作结果，0表示成功，其他值表示失败
*/
uint32_t coze_main_intercom_push_key(struct key_callback_list_s *callback_list, uint32_t keyvalue, uint32_t extern_value)
{
    static uint8 key_state = 0;
    if ((keyvalue >> 8) != AD_PRESS)
    { return 0; }
    uint32 key_val = (keyvalue & 0xff);
    if ((key_val == KEY_EVENT_LDOWN) || (key_val == KEY_EVENT_REPEAT)) {
        if (key_state == 0) {
            os_printf("key start\r\n");
            key_state = 1;
            coze_mgr.key_triggered = 1;
        }
    } else if ((key_val == KEY_EVENT_LUP)) {
        if (key_state == 1) {
            os_printf("key stop\r\n");
            key_state = 0;
            coze_mgr.key_triggered = 0;
        }
    }
    return 0;
}

const char *coze_main_state_str(coze_demo_state state)
{
    switch (state) {
        case COZE_DEMO_STATE_IDLE:
            return "IDLE";
        case COZE_DEMO_STATE_CONNECTED:
            return "CONNECTED";
        case COZE_DEMO_STATE_DISCONNECTED:
            return "DISCONNECTED";
        case COZE_DEMO_STATE_READY:
            return "READY";
        case COZE_DEMO_STATE_WAITING:
            return "WAITING";
        case COZE_DEMO_STATE_PUSHING:
            return "PUSHING";
        default:
            return "INVALID";
    }
    return "INVALID";
}

/*
 * @brief 获取当前状态
 * @return 当前状态
*/
static coze_demo_state coze_main_get_state(void)
{
    coze_demo_state state = COZE_DEMO_STATE_IDLE;
    os_mutex_lock(&coze_mgr.lock, osWaitForever);
    state = coze_mgr.state;
    os_mutex_unlock(&coze_mgr.lock);
    return state;
}

/*
 * @brief 设置当前状态
 * @param new_state 新状态
*/
static void coze_main_set_state(coze_demo_state new_state)
{
    int32 changed = 0;
    if (coze_mgr.state == new_state) { return; }
    os_mutex_lock(&coze_mgr.lock, osWaitForever);
    switch (new_state) {
        case COZE_DEMO_STATE_IDLE:
            changed = 1;
            break;
        case COZE_DEMO_STATE_CONNECTED:
            changed = 1;
            break;
        case COZE_DEMO_STATE_DISCONNECTED:
            coze_mgr.time_stamp = UINT64_MAX;
            changed = 1;
            break;
        case COZE_DEMO_STATE_READY:
            if (coze_mgr.connected) {
                changed = 1;
            }
            break;
        case COZE_DEMO_STATE_WAITING:
            coze_mgr.time_stamp = UINT64_MAX;
            if (coze_mgr.connected) {
                changed = 1;
            }
            break;
        case COZE_DEMO_STATE_PUSHING:
            if (coze_mgr.connected) {
                changed = 1;
            }
            break;
        default:
            break;
    }
    if (changed) {
        os_printf("New State:%s -> %s\r\n", coze_main_state_str(coze_mgr.state), coze_main_state_str(new_state));
        coze_mgr.state = new_state;
    } else {
        os_printf("invalid state: %s -> %s\r\n", coze_main_state_str(coze_mgr.state), coze_main_state_str(new_state));
    }
    os_mutex_unlock(&coze_mgr.lock);
}

/*
 * @brief 超时警告函数
 * @param 无
*/
static void coze_main_timeout_warn(void)
{
    os_printf("**当前网络较差，请稍等!**\n");
    coze_audio_play_prompt_tone(wangluocha, wangluocha_size, OPUS_PROMPT_FRAME_LEN);
    os_sleep_ms(3000);
}

/*
 * @brief 超时重连函数
 * @param 无
*/
static void coze_main_timeout_reset(void)
{
    os_printf("**网络超时，即将重连!**\n");
    coze_audio_play_prompt_tone(chaoshi, chaoshi_size, OPUS_PROMPT_FRAME_LEN);
    os_sleep_ms(3000);
    coze_main_set_state(COZE_DEMO_STATE_IDLE);
    llm_sts_reconnect(coze_mgr.sts_session);
}

/*
 * @brief 等待进入监听函数
 * @param 无
 * @return 操作结果，0表示成功，其他值表示失败
*/
static int32 coze_main_waiting(void)
{
    int32 ret = RET_OK;
    uint32 timeout = 0;
    do {
        ret = llm_sts_wait(coze_mgr.sts_session, 500);
        if (ret == RET_ERR) {
            os_printf("This can be confirmed to be a disconnection!\r\n");
            break;
        } else if (ret == LLME_WAIT_TIMEOUT) {
            timeout++;
            if (timeout == 6) {
                coze_main_timeout_warn();
            }
            if (timeout > 10) {
                coze_main_timeout_reset();
                break;
            }
        }
    } while (ret == LLME_WAIT_TIMEOUT);
    return ret;
}

/*
 * @brief 事件回调函数(不可阻塞)
 * @param session 会话指针
 * @param evt 事件类型
 * @param param1 事件参数1
 * @param param2 事件参数2
 * @return 操作结果，0表示成功，其他值表示失败
*/
int32 coze_main_event_cb(void *session, uint16 evt, uint32 param1, uint32 param2)
{
    int ret = RET_OK;
    uint8 process = 0;
    struct coze_demo_event_msg event_msg = {
        .event      = evt,
        .msg        = NULL,
        .msg_len    = 0,
    };
    switch (evt) {
        case LLM_EVENT_CONNECTED: {
            process = 1;
            break;
        }
        case LLM_EVENT_DISCONNECT: {
            process = 1;
            break;
        }
        case LLM_EVENT_STT_RESULT: {
            event_msg.msg = llm_strdup((const char *)param1);
            if (event_msg.msg == NULL) {
                os_printf("stt result no memory!\r\n");
                break;
            }
            event_msg.msg_len = param2;
            process = 1;
            break;
        }
        case LLM_EVENT_TTS_RESULT: {
            event_msg.msg = llm_strdup((const char *)param1);
            if (event_msg.msg == NULL) {
                os_printf("tts result no memory!\r\n");
                break;
            }
            event_msg.msg_len = param2;
            process = 1;
            break;
        }
        case LLM_EVENT_VAD_RESULT: {
            event_msg.msg = llm_strdup((const char *)(param1 == COZE_SERVER_VAD_START ? "START" : "STOP"));
            if (event_msg.msg == NULL) {
                os_printf("vad result no memory!\r\n");
                break;
            }
            event_msg.msg_len = os_strlen(event_msg.msg);
            process = 1;
            break;
        }
        case LLM_EVENT_DIALOGUE_END: {
            process = 1;
            break;
        }
        case LLM_EVENT_DIALOGUE_TIMEOUT: {
            process = 1;
            break;
        }
        case LLM_EVENT_ERROR_MSG: {
            event_msg.msg = llm_strdup((const char *)param1);
            if (event_msg.msg == NULL) {
                os_printf("err msg no memory!\r\n");
                break;
            }
            event_msg.msg_len = param2;
            process = 1;
            break;
        }
        case LLM_EVENT_CUSTOMIZE: {
            event_msg.msg = llm_strdup((const char *)param1);
            if (event_msg.msg == NULL) {
                os_printf("customize no memory!\r\n");
                break;
            }
            event_msg.msg_len = param2;
            process = 1;
            break;
        }
        case LLM_EVENT_UPLOAD_FILE_RESULT: {
            event_msg.msg = llm_strdup((const char *)param1);
            if (event_msg.msg == NULL) {
                os_printf("customize no memory!\r\n");
                break;
            }
            event_msg.msg_len = param2;
            process = 1;
            break;
        }
        case LLM_EVENT_TTI_RESULT: {
            event_msg.msg = llm_strdup((const char *)param1);
            if (event_msg.msg == NULL) {
                os_printf("customize no memory!\r\n");
                break;
            }
            event_msg.msg_len = param2;
            process = 1;
            break;
        }
        default:
            break;
    }
    if (process) {
        RB_INT_SET(&coze_mgr.event_queue, event_msg);
    }
    return ret;
}

/*
 * @brief 系统无线事件回调函数
 * @param event_id 事件ID
 * @param data 事件数据
 * @param priv 私有数据
 * @return 操作结果，0表示成功，其他值表示失败
*/
sysevt_hdl_res coze_main_wifi_event(uint32 event_id, uint32 data, uint32 priv)
{
    switch (event_id & 0xffff) {
        case SYSEVT_WIFI_DISCONNECT:
            os_printf("**网络异常!**\r\n", event_id);
            break;
        case SYSEVT_WIFI_CONNECTTED:
            coze_main_set_state(COZE_DEMO_STATE_IDLE);
            llm_sts_reconnect(coze_mgr.sts_session);
            break;
    }
    return SYSEVT_CONTINUE;
}

/*
 * @brief 系统网络事件回调函数
 * @param event_id 事件ID
 * @param data 事件数据
 * @param priv 私有数据
 * @return 操作结果，0表示成功，其他值表示失败
*/
sysevt_hdl_res coze_main_network_event(uint32 event_id, uint32 data, uint32 priv)
{
    struct netif *nif;

    switch (event_id) {
        case SYS_EVENT(SYS_EVENT_NETWORK, SYSEVT_LWIP_DHCPC_DONE):
            nif = netif_find("w0");
            gethostbyname_async("ws.coze.cn");
            gethostbyname_async("lf-bot-studio-plugin-resource.coze.cn");
            if (coze_mgr.ip_addr != nif->ip_addr.addr && sys_status.dhcpc_done) {
                coze_mgr.ip_addr = nif->ip_addr.addr;
                coze_main_set_state(COZE_DEMO_STATE_IDLE);
                llm_sts_reconnect(coze_mgr.sts_session);
            }
            break;
    }
    return SYSEVT_CONTINUE;
}

/*
 * @brief 应用初始化函数
 * @param llm_name LLM名称
 * @return 操作结果，0表示成功，其他值表示失败
*/
int32 coze_main_app_init(char *llm_name)
{
    int ret = 0;
    struct coze_demo_event_msg *event_queue_buf = NULL;
    llm_global_init(&global);

    coze_mgr.sts_session = llm_sts_init(llm_name, &coze_sts_session_cfg);
    if (!coze_mgr.sts_session) {
        os_printf("sts session init fail!\r\n");
        return RET_ERR;
    }

    ret = llm_sts_config(coze_mgr.sts_session, LLM_CONFIG_TYPE_TRANS,
                         (void *)&coze_sts_trans_cfg, sizeof(coze_sts_trans_cfg));
    if (ret) {
        os_printf("sts_trans_cfg fail!\r\n");
        return RET_ERR;
    }

    coze_sts_platform_cfg.turn_detection_type = "client_interrupt";
    ret = llm_sts_config(coze_mgr.sts_session, LLM_CONFIG_TYPE_MODEL,
                         (void *)&coze_sts_platform_cfg, sizeof(coze_sts_platform_cfg));
    if (ret) {
        os_printf("sts_trans_cfg fail!\r\n");
        return RET_ERR;
    }

    OS_TASK_INIT("COZE_AUPLAY", &coze_audio_play_task, coze_audio_play_thread, NULL, OS_TASK_PRIORITY_NORMAL, 8192);

    coze_audio_play_prompt_tone(kaiji, kaiji_size, OPUS_PROMPT_FRAME_LEN);
    os_sleep_ms(1000);

    sys_event_take(SYS_EVENT(SYS_EVENT_WIFI, 0), coze_main_wifi_event, 0);
    sys_event_take(SYS_EVENT(SYS_EVENT_NETWORK, 0), coze_main_network_event, 0);

    add_keycallback(coze_main_intercom_push_key, NULL);
#ifdef LLM_SPV12XX
    OS_TASK_INIT("COZE_AWAKEN", &coze_main_task_awaken, coze_main_awaken_demo, NULL, OS_TASK_PRIORITY_NORMAL + 1, 1024);
#endif
    coze_mgr.stream = open_stream_available(R_SPEECH_RECOGNITION, 0, 8, coze_audio_opcode_func, NULL);
    if (!coze_mgr.stream) {
        os_printf("open speech_recognition stream err!\r\n");
        return RET_ERR;
    }

    coze_mgr.aualaw_dev = (struct aualaw_device *)dev_get(HG_AUALAW_DEVID);
    ret = aualaw_open(coze_mgr.aualaw_dev);
    if (ret != 0) {
        os_printf("aualaw open err!\n");
        return RET_ERR;
    }

    os_mutex_init(&coze_mgr.lock);

#ifdef COZE_DEMO_URL_PLAY
    urlfile_init(coze_ufprotos, ARRAY_SIZE(coze_ufprotos));
#endif

    event_queue_buf = llm_malloc((COZE_EVENT_QUEUE_CNT + 1) * sizeof(struct coze_demo_event_msg));
    if (!event_queue_buf) {
        os_printf("event_queue_buf malloc fail, no memory!\r\n");
        return RET_ERR;
    }
    RB_INIT_R(&coze_mgr.event_queue, COZE_EVENT_QUEUE_CNT, event_queue_buf);

    os_printf("Waiting for network connection...");
    while (!sys_status.wifi_connected || !sys_status.dhcpc_done) {
        _os_printf(".");
        os_sleep(1);
    }
    os_printf("Network connected!\r\n");
    struct netif *nif = netif_find("w0");
    coze_mgr.ip_addr = nif->ip_addr.addr;

    coze_main_set_state(COZE_DEMO_STATE_IDLE);
    llm_sts_reconnect(coze_mgr.sts_session);
    return ret;
}

/*
 * @brief: 处理Coze AI错误消息
 * @param: char *error 错误消息
 * @return: int32
*/
static int32 coze_main_error_process(char *error)
{
    int32 err_code = 0;
    char *err_msg = NULL;

    os_printf("**设备异常**\n");
    if (error) {
        cJSON *root = cJSON_Parse(error);
        if (root == NULL) {
            os_printf("Error before: %s\n", error);
            return RET_ERR;
        }

        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsObject(data)) {
            cJSON *code_item = cJSON_GetObjectItemCaseSensitive(data, "code");
            cJSON *msg_item = cJSON_GetObjectItemCaseSensitive(data, "msg");

            if (cJSON_IsNumber(code_item) && cJSON_IsString(msg_item)) {
                err_code = code_item->valueint;
                err_msg = msg_item->valuestring;
            }
        }

        if (err_code == 0 && err_msg == NULL) {
            os_printf("Error after: %s\n", error);
            cJSON_Delete(root);
            return RET_ERR;
        }
        os_printf("\"%d\": \"%s\"\r\n", err_code, err_msg);
        switch (err_code) {
            case 4302: {
                os_printf("**没听清楚，您再说一遍!**\n");
                coze_mgr.finish = 1;
                coze_main_set_state(COZE_DEMO_STATE_WAITING);
                break;
            }
            default: {
                llm_sts_stop(coze_mgr.sts_session);
                // 注意：所有调用 llm_sts_reconnect 前必须将状态设置为 COZE_DEMO_STATE_IDLE
                //coze_main_set_state(COZE_DEMO_STATE_IDLE);
                //llm_sts_reconnect(coze_mgr.sts_session);
                break;
            }
        }
        cJSON_Delete(root);
    }
    return RET_OK;
}

#ifdef COZE_DEMO_URL_PLAY
/*
 * @brief: 获取Coze AI音频URL
 * @param: char *data 音频数据
 * @param: uint32 data_len 音频数据长度
 * @return: char * 音频URL
*/
static char *coze_main_get_audio_url(char *data, uint32 data_len)
{
    // TODO
    return NULL;
}
#endif

static int32 coze_main_customize_process(char *data, uint32 data_len)
{
#ifdef COZE_DEMO_URL_PLAY
    char *audio_url = coze_main_get_audio_url(data, data_len);
    if (audio_url) {
        coze_mgr.uf_hdl = uf_open(audio_url, "ra", 0);
        coze_mgr.finish = 0;
        coze_mgr.time_stamp = os_jiffies();
        llm_free(audio_url);
    }
#endif
    return RET_OK;
}

static int32 coze_main_file_upload_process(char *data, uint32 data_len)
{
    cJSON *json = cJSON_Parse(data);
    int32 ret = RET_OK;

    if (json == NULL) {
        const char *errorPtr = cJSON_GetErrorPtr();
        if (errorPtr != NULL) {
            os_printf("%s:Json error before: %s\n", errorPtr);
        }
        return RET_ERR;
    }

    cJSON *pdata = cJSON_GetObjectItem(json, "data");
    if (pdata == NULL) {
        os_printf("No 'data' found in JSON.\n");
        cJSON_Delete(json);
        return RET_ERR;
    }

    cJSON *id = cJSON_GetObjectItem(pdata, "id");
    if (id != NULL && cJSON_IsString(id)) {
        os_printf("get file id: [%s]\n", id->valuestring);//获取file_ID字符串
        coze_sts_platform_cfg.file_id = llm_strdup(id->valuestring);
        ret = llm_sts_config(coze_mgr.sts_session, LLM_CONFIG_TYPE_MODEL,
                             (void *)&coze_sts_platform_cfg, sizeof(coze_sts_platform_cfg));//填入config并update到llm库中
        if (ret) {
            os_printf("sts_model_cfg fail!\r\n");
            return RET_ERR;
        }
    } else {
        os_printf("'id' field not found or is not a string\n");
    }

    cJSON_Delete(json);
    return RET_OK;
}

static int32 coze_main_tti_process(char *data, uint32 data_len)
{
    //目前只是将分析的image_url打印出来，还需要用户通过https去该url拉数据。
    cJSON *json = cJSON_Parse(data);
    if (json == NULL) {
        os_printf("parse json error!\n");
        return RET_ERR;
    }

    // 查找 "data" 对象
    cJSON *json_data = cJSON_GetObjectItem(json, "data");
    if (json_data == NULL || !cJSON_IsObject(json_data)) {
        os_printf("can not find data obj!\n");
        cJSON_Delete(json);
        return RET_ERR;
    }

    // 查找 "content" 字段
    cJSON *content_item = cJSON_GetObjectItem(json_data, "content");
    if (content_item == NULL || !cJSON_IsString(content_item) || (content_item->valuestring == NULL)) {
        os_printf("can not find content obj!\n");
        cJSON_Delete(json);
        return RET_ERR;
    }

    // 解析 content 的 JSON 字符串
    cJSON *content_json = cJSON_Parse(content_item->valuestring);
    if (content_json == NULL) {
        os_printf("can not find content val str!\n");
        cJSON_Delete(json);
        return RET_ERR;
    }

    // 查找 "data" 对象中的 "image_urls" 数组
    cJSON *image_urls = cJSON_GetObjectItem(content_json, "data");
    if (image_urls == NULL || !cJSON_IsObject(image_urls)) {
        os_printf("can not find content data!\n");
        cJSON_Delete(content_json);
        cJSON_Delete(json);
        return RET_ERR;
    }

    // 从 "data" 对象中获取 "image_urls" 数组
    cJSON *urls_array = cJSON_GetObjectItem(image_urls, "image_urls");
    if (urls_array == NULL || !cJSON_IsArray(urls_array)) {
        os_printf("can not find image url array!\n");
        cJSON_Delete(content_json);
        cJSON_Delete(json);
        return RET_ERR;
    }

    // 遍历数组，提取 HTTPS 链接
    int array_size = cJSON_GetArraySize(urls_array);
    for (int i = 0; i < array_size; i++) {
        cJSON *url_item = cJSON_GetArrayItem(urls_array, i);
        if (cJSON_IsString(url_item) && (url_item->valuestring != NULL)) {
            os_printf("get coze gen image url:%s\n", url_item->valuestring);
        }
    }
    // 释放 JSON 对象
    cJSON_Delete(content_json);
    cJSON_Delete(json);
    return RET_OK;
}


/*
 * @brief: 处理Coze AI事件消息
 * @param: void
 * @return: int32
*/
static int32 coze_main_event_msg_process(void)
{
    struct coze_demo_event_msg event_msg = {0};

    RB_INT_GET(&coze_mgr.event_queue, event_msg);
    if (event_msg.event == LLM_EVENT_UNKNOWN) { return RET_OK; }

    switch (event_msg.event) {
        case LLM_EVENT_CONNECTED: {
            os_printf("Coze AI connected!\r\n");
            coze_main_set_state(COZE_DEMO_STATE_CONNECTED);
            break;
        }
        case LLM_EVENT_DISCONNECT: {
            os_printf("Coze AI disconnect!\r\n");
            coze_main_set_state(COZE_DEMO_STATE_DISCONNECTED);
            break;
        }
        case LLM_EVENT_STT_RESULT: {
            os_printf("COZE AI STT(%d):", event_msg.msg_len);
            hgprintf_out(event_msg.msg, event_msg.msg_len, 0);
            _os_printf("\r\n");
            llm_free(event_msg.msg);
            break;
        }
        case LLM_EVENT_TTS_RESULT: {
            os_printf("COZE AI TTS(%d):", event_msg.msg_len);
            hgprintf_out(event_msg.msg, event_msg.msg_len, 0);
            _os_printf("\r\n");
            llm_free(event_msg.msg);
            coze_mgr.time_stamp = os_jiffies();
            break;
        }
        case LLM_EVENT_VAD_RESULT: {
            os_printf("Coze AI recv server vad result:%s\r\n", event_msg.msg);
            if (os_strncmp(event_msg.msg, "STOP", event_msg.msg_len) == 0) {
                coze_main_set_state(COZE_DEMO_STATE_READY);
            }
            llm_free(event_msg.msg);
            break;
        }
        case LLM_EVENT_DIALOGUE_END: {
            os_printf("COZE AI check finish!\r\n");
            coze_mgr.finish = 1;
            coze_mgr.time_stamp = os_jiffies();
            break;
        }
        case LLM_EVENT_DIALOGUE_TIMEOUT: {
            os_printf("COZE AI dialogue timeout!\r\n");
            coze_mgr.dialogue_timeout_cnt++;    //1s
            if (coze_mgr.dialogue_timeout_cnt > COZE_DEMO_DIALOGUE_TIMEOUT_MAX_CNT) {
                coze_mgr.dialogue_timeout_cnt = 0;
                llm_sts_interrupt(coze_mgr.sts_session, 0);
                coze_mgr.finish = 1;
                coze_mgr.time_stamp = os_jiffies();
                os_printf("End the current conversation and start a new one!\r\n");
            }
            break;
        }
        case LLM_EVENT_ERROR_MSG: {
            os_printf("COZE AI recv err msg(%d):", event_msg.msg_len);
            hgprintf_out(event_msg.msg, event_msg.msg_len, 0);
            _os_printf("\r\n");
            coze_main_error_process(event_msg.msg);
            llm_free(event_msg.msg);
            break;
        }
        case LLM_EVENT_CUSTOMIZE: {
            os_printf("COZE AI recv customize(%d):", event_msg.msg_len);
            hgprintf_out(event_msg.msg, event_msg.msg_len, 0);
            _os_printf("\r\n");
            coze_main_customize_process(event_msg.msg, event_msg.msg_len);
            llm_free(event_msg.msg);
            break;
        }
        case LLM_EVENT_UPLOAD_FILE_RESULT: {
            os_printf("COZE AI recv upload file result(%d):", event_msg.msg_len);
            hgprintf_out(event_msg.msg, event_msg.msg_len, 0);
            _os_printf("\r\n");
            coze_main_file_upload_process(event_msg.msg, event_msg.msg_len);
            llm_free(event_msg.msg);
            break;
        }
        case LLM_EVENT_TTI_RESULT: {
            os_printf("COZE AI recv tti result(%d):", event_msg.msg_len);
            hgprintf_out(event_msg.msg, event_msg.msg_len, 0);
            _os_printf("\r\n");
            coze_main_tti_process(event_msg.msg, event_msg.msg_len);
            llm_free(event_msg.msg);
            break;
        }
        default: {
            os_printf("Not supported event msg: %d\r\n", event_msg.event);
            break;
        }
    }

    return RET_OK;
}

/*
 * @brief 主线程
*/
void coze_main_demo(void)
{
    int32 ret = RET_OK;
    coze_demo_state state = COZE_DEMO_STATE_IDLE;
    uint64 jiff = 0;
    uint8 last_key_triggered = 0;   // 记录上一次按键状态；
    uint8 dialogue_mode = 0;        // 对话模式，0为按键模式，1为语音模式；
    uint8 send_complete = 0;        // 按键模式下，需发送完成帧告知服务器音频数据上传结束；

    ret = coze_main_app_init("coze_sts");
    if (ret) {
        os_printf("coze main init failed: %d\r\n", ret);
        goto cleanup;
    }

    while (1) {
        coze_main_event_msg_process();

        if (coze_mgr.connected == 1) {
            // 处理语音触发事件
            if (coze_mgr.voice_triggered) {
                coze_mgr.voice_triggered = 0;
                if (dialogue_mode != DIALOGUE_MODE_VOICE) {
                    coze_sts_platform_cfg.turn_detection_type = "server_vad";
                    llm_sts_config(coze_mgr.sts_session, LLM_CONFIG_TYPE_MODEL,
                                   (void *)&coze_sts_platform_cfg, sizeof(coze_sts_platform_cfg));
                    dialogue_mode = DIALOGUE_MODE_VOICE;
                }
                coze_mgr.finish = 0;
#ifdef COZE_DEMO_URL_PLAY
                uf_close(coze_mgr.uf_hdl);
                coze_mgr.uf_hdl = NULL;
#endif
                coze_main_set_state(COZE_DEMO_STATE_WAITING);
            }
            // 处理按键触发事件，优先级：按键 > 语音
            if (last_key_triggered != coze_mgr.key_triggered) {
                // 按键按下
                if (coze_mgr.key_triggered == 1) {
                    if (dialogue_mode != DIALOGUE_MODE_KEYBOARD) {
                        // 按键配置与语音配置不同（turn_detection_type），需更新
                        coze_sts_platform_cfg.turn_detection_type = "client_interrupt";
                        llm_sts_config(coze_mgr.sts_session, LLM_CONFIG_TYPE_MODEL,
                                       (void *)&coze_sts_platform_cfg, sizeof(coze_sts_platform_cfg));
                        dialogue_mode = DIALOGUE_MODE_KEYBOARD;
                    }
#ifdef COZE_DEMO_URL_PLAY
                    uf_close(coze_mgr.uf_hdl);
                    coze_mgr.uf_hdl = NULL;
#endif
                    coze_main_set_state(COZE_DEMO_STATE_WAITING);
                }
                // 按键松开
                else {
                    // 若发送过音频数据需发送完成帧
                    if (coze_mgr.send_audio_cnt > 0) {
                        send_complete = 1;
                    }
                    coze_main_set_state(COZE_DEMO_STATE_READY);
                }
                last_key_triggered = coze_mgr.key_triggered;
            }
            // 处理语音触发模式下自动退出多轮对话
            if (dialogue_mode == DIALOGUE_MODE_VOICE) {
                jiff = os_jiffies() - coze_mgr.time_stamp;
                if (os_jiffies_to_msecs(jiff) > COZE_IDLE_TIMEOUT && coze_mgr.time_stamp != UINT64_MAX && llm_sts_check_audio(coze_mgr.sts_session)) {
                    llm_sts_interrupt(coze_mgr.sts_session, 0);
                    os_printf("**自动退出**\r\n");
                    coze_audio_play_prompt_tone(tuixia, tuixia_size, OPUS_PROMPT_FRAME_LEN);
                    coze_mgr.finish = 0;
                    coze_mgr.time_stamp = UINT64_MAX;
#ifdef COZE_DEMO_URL_PLAY
                    uf_close(coze_mgr.uf_hdl);
                    coze_mgr.uf_hdl = NULL;
#endif
                    coze_main_set_state(COZE_DEMO_STATE_READY);
                }
            }
        }
        state = coze_main_get_state();
        switch (state) {
            case COZE_DEMO_STATE_IDLE:
                break;
            case COZE_DEMO_STATE_CONNECTED: {
                os_printf("**已连接!**\n");
                coze_mgr.connected = 1;
                coze_audio_play_prompt_tone(yilianjie, yilianjie_size, OPUS_PROMPT_FRAME_LEN);
                os_sleep_ms(1000);
                // 连接成功后，需更新一次配置
                if (dialogue_mode == DIALOGUE_MODE_VOICE) {
                    coze_sts_platform_cfg.turn_detection_type = "server_vad";
                } else if (dialogue_mode == DIALOGUE_MODE_KEYBOARD) {
                    coze_sts_platform_cfg.turn_detection_type = "client_interrupt";
                }
                llm_sts_config(coze_mgr.sts_session, LLM_CONFIG_TYPE_MODEL,
                               (void *)&coze_sts_platform_cfg, sizeof(coze_sts_platform_cfg));
                coze_main_set_state(COZE_DEMO_STATE_READY);
                break;
            }
            case COZE_DEMO_STATE_READY: {
#ifdef COZE_DEMO_URL_PLAY
                if (coze_mgr.uf_hdl) {
                    if (uf_eof(coze_mgr.uf_hdl)) {
                        os_printf("UF EOF! close it!\r\n");
                        uf_close(coze_mgr.uf_hdl);
                        coze_mgr.uf_hdl = NULL;
                        coze_mgr.finish = 1;
                    } else {
                        coze_mgr.uf_audio_len = 0;
                        uf_ioctl(coze_mgr.uf_hdl, UF_IOCTL_CMD_GET_DATASIZE, (uint32)&coze_mgr.uf_audio_len, 0);
                        coze_mgr.uf_audio_len = (coze_mgr.uf_audio_len / OPUS_PROMPT_FRAME_LEN) * OPUS_PROMPT_FRAME_LEN;
                        if (coze_mgr.uf_audio_len >= OPUS_PROMPT_FRAME_LEN) {
                            coze_mgr.uf_audio_buf = llm_malloc(coze_mgr.uf_audio_len + 1);
                            if (coze_mgr.uf_audio_buf) {
                                ret = uf_read(coze_mgr.uf_audio_buf, coze_mgr.uf_audio_len, 1, coze_mgr.uf_hdl);
                                if (ret > 0) {
                                    coze_audio_play_opus(coze_mgr.uf_audio_buf, coze_mgr.uf_audio_len, OPUS_PROMPT_FRAME_LEN);
                                }
                                llm_free(coze_mgr.uf_audio_buf);
                            } else {
                                os_printf("uf_audio_buf malloc fail, no memory!\r\n");
                            }
                        }
                    }
                }
#endif
                // 按键触发模式
                if (dialogue_mode == DIALOGUE_MODE_KEYBOARD) {
                    if (send_complete) {
                        ret = llm_sts_send(coze_mgr.sts_session, LLM_DATA_TYPE_AUDIO, LLM_DATA_STATE_END, NULL, 0);
                        if (ret == RET_OK) {
                            send_complete = 0;
                            coze_audio_play_prompt_tone(fasong, fasong_size, OPUS_PROMPT_FRAME_LEN);
                        } else if (ret != LLME_AGAIN) {
                            os_printf("send_complete fail!\r\n");
                            coze_main_set_state(COZE_DEMO_STATE_IDLE);
                            llm_sts_reconnect(coze_mgr.sts_session);
                            break;
                        }
                    }
                }
                // 语音触发模式
                else if (dialogue_mode == DIALOGUE_MODE_VOICE) {
                    // 自动触发多轮对话
                    if (coze_mgr.finish && llm_sts_check_audio(coze_mgr.sts_session)) {
                        coze_main_set_state(COZE_DEMO_STATE_WAITING);
                        os_printf("**新对话：**\r\n");
                    }
                }
                break;
            }
            case COZE_DEMO_STATE_WAITING: {
                // 打断上一次对话
                llm_sts_interrupt(coze_mgr.sts_session, 0);
                // 等待服务器进入监听状态
                ret = coze_main_waiting();
                if (ret != RET_OK) {
                    break;
                }
                if (dialogue_mode == DIALOGUE_MODE_VOICE) {
                    if (coze_mgr.finish == 0) {
                        os_printf("**我在，请说：**\r\n");
                        coze_audio_play_prompt_tone(wozai, wozai_size, OPUS_PROMPT_FRAME_LEN);
                        os_sleep_ms(1000);
                    }
                    coze_mgr.finish = 0;
                    coze_mgr.time_stamp = os_jiffies();
                }
                coze_audio_play_prompt_tone(beep, beep_size, OPUS_PROMPT_FRAME_LEN);
                coze_audio_free_sample_stream(coze_mgr.stream);
                coze_main_set_state(COZE_DEMO_STATE_PUSHING);
                coze_mgr.send_audio_cnt = 0;
                break;
            }
            case COZE_DEMO_STATE_PUSHING: {
                // 发送音频数据
                coze_audio_send_data(coze_mgr.stream, coze_mgr.aualaw_dev);
                break;
            }
            case COZE_DEMO_STATE_DISCONNECTED: {
                os_printf("**已断开!**\n");
                coze_mgr.connected = 0;
                coze_audio_play_prompt_tone(yiduankai, yiduankai_size, OPUS_PROMPT_FRAME_LEN);
                os_sleep_ms(1000);
                coze_mgr.finish = 0;
                coze_mgr.key_triggered = 0;
                last_key_triggered = 0;
                send_complete = 0;
                coze_mgr.voice_triggered = 0;
                coze_main_set_state(COZE_DEMO_STATE_IDLE);
                break;
            }
            default:
                break;
        }
        os_sleep_ms(10);
    }

cleanup:
    if (coze_mgr.psram_copy_buff) { llm_free(coze_mgr.psram_copy_buff); }
    if (coze_mgr.aualaw_buf) { llm_free(coze_mgr.aualaw_buf); }
#ifdef COZE_DEMO_URL_PLAY
    if (coze_mgr.uf_audio_buf) { llm_free(coze_mgr.uf_audio_buf); }
    uf_close(coze_mgr.uf_hdl);
#endif
    llm_free(coze_mgr.event_queue.rbq);
    llm_sts_deinit(coze_mgr.sts_session);
    llm_global_deinit();
}

/*******************************************************
从SD卡上传图片sample code,若要使用LLM视觉处理功能:
1.需要在project_config.h打开如下宏：
#define DVP_EN                          1
#define SDH_EN                          1
#define FS_EN                           1
#define OPENDML_EN                      1

2.将config-taixin.h中将#define CURL_DISABLE_MIME 屏蔽掉

3.确保coze智能体搭建支持视觉处理

4.上传完成后，llm库会返回event:LLM_EVENT_UPLOAD_FILE_RESULT
从event的参数解析file_id(可参考coze_main_file_upload_process)，
用户可以保存fileid,并将需要跟智能体交互的fileid通过llm_sts_config更新到llm库

*******************************************************/

int32 coze_atcmd_upload_photo(const char *cmd, char *argv[], uint32 argc)
{
    char *cache_buf = NULL;
    const char *update_filename = NULL;
    void *fp = NULL;
    uint32_t filesize = 0;
    uint32_t file_tot_size = 0;
    uint32_t readsize = SD_CACHE_SIZE;
    uint32_t bytes_read = 0;
    int32 ret = 0;

    if (argc < 1) {
        os_printf("argv is too less\n");
        return RET_ERR;
    }
    update_filename = argv[0];
    fp = osal_fopen(update_filename, "rb");
    if (!fp) {
        os_printf("%s file not exist\n", update_filename);
        ret = RET_ERR;
        goto __atcmd_update_file_end;
    }

    filesize = osal_fsize(fp);
    file_tot_size = filesize;
    cache_buf = llm_malloc(SD_CACHE_SIZE);
    if (!cache_buf) {
        ret = RET_ERR;
        goto __atcmd_update_file_end;
    }
    os_printf("%s:filesize:%d,cache_buff:0x%x\n", __FUNCTION__, filesize, cache_buf);

    while (filesize) {
        if (filesize < SD_CACHE_SIZE) {
            readsize = filesize;
        } else {
            readsize = SD_CACHE_SIZE;
        }
        bytes_read = osal_fread(cache_buf, 1, readsize, fp);
        if (bytes_read == 0) {
            os_printf("Read file error\n");
            ret = RET_ERR;
            goto __atcmd_update_file_end;
        }

        if (filesize == file_tot_size) { // First chunk
            llm_sts_upload_file(coze_mgr.sts_session, LLM_DATA_TYPE_FILE, LLM_DATA_STATE_START, NULL, file_tot_size);
        }
        llm_sts_upload_file(coze_mgr.sts_session, LLM_DATA_TYPE_FILE, LLM_DATA_STATE_MIDDLE, cache_buf, bytes_read);
        filesize -= bytes_read;
    }
    llm_sts_upload_file(coze_mgr.sts_session, LLM_DATA_TYPE_FILE, LLM_DATA_STATE_END, NULL, 0);
    ret = RET_OK;

__atcmd_update_file_end:
    if (fp) {
        osal_fclose(fp);
    }
    if (cache_buf) {
        llm_free(cache_buf);
    }
    return ret;
}

void coze_demo(void)
{
    OS_TASK_INIT("COZE_DEMO", &coze_main_task_demo, coze_main_demo, NULL, OS_TASK_PRIORITY_NORMAL, 2048);
}
#endif
