#include "sys_config.h"
#include "typesdef.h"
#include "osal/task.h"
#include "osal/string.h"
#include "dev.h"
#include "devid.h"
#include "hal/audac.h"
#include "hal/gpio.h"
#include "stream_frame.h"
#include "audio_dac.h"
#include "custom_mem/custom_mem.h"
#include "osal/semaphore.h"

#include "printer_voice.h"
#include "voice_data.h"


/* ========== configuration ========== */
#define VOICE_BUF_SIZE      1024
#define VOICE_BUF_COUNT     4
#define VOICE_TASK_STACK    1024
#define VOICE_TASK_PRIO     OS_TASK_PRIORITY_NORMAL

/* 8002D amplifier enable pin (PC4_PA_EN on schematic) */
#define PA_EN_PIN           PC_4

/* ========== audio clip index ========== */
typedef struct {
    const uint8_t *wav_data;     /* WAV data (includes 44-byte header) */
    uint32_t       wav_len;      /* total WAV data length in bytes */
    uint16_t       sample_rate;  /* sample rate */
    uint8_t        bits_per_sample;
    uint8_t        channels;
    const char    *name;         /* debug name */
} voice_clip_t;

#define WAV_HEADER_SIZE  44

/*
 * 6 mode audio clip index
 * _len are extern linkage constants, not compile-time constants,
 * so we fill wav_len at runtime in printer_voice_init().
 */
static voice_clip_t g_clips[MODE_COUNT] = {
    /* MODE_SKETCH_PEN  */ { jianbihua_wav,  0, 8000, 16, 1, "jianbihua"  },
    /* MODE_LINE_ART    */ { xiangao_wav,    0, 8000, 16, 1, "xiangao"    },
    /* MODE_PENCIL      */ { sumiao_wav,     0, 8000, 16, 1, "sumiao"     },
    /* MODE_ANIME       */ { dongman_wav,    0, 8000, 16, 1, "dongman"    },
    /* MODE_CARTOON     */ { katong_wav,     0, 8000, 16, 1, "katong"     },
    /* MODE_INK_WASH    */ { shuimohua_wav,  0, 8000, 16, 1, "shuimohua"  },
};

/* ========== 8002D amplifier control ========== */

static void voice_pa_enable(void)
{
    gpio_set_mode(PA_EN_PIN, GPIO_PULL_NONE, GPIO_PULL_LEVEL_NONE);
	gpio_set_dir(PA_EN_PIN, GPIO_DIR_OUTPUT);
    gpio_set_val(PA_EN_PIN, 0);
}

static void voice_pa_disable(void)
{
    gpio_set_val(PA_EN_PIN, 1);
}

/* ========== playback state ========== */
static volatile uint8_t  g_voice_busy   = 0;
static volatile uint8_t  g_voice_abort  = 0;
static printer_mode_t    g_pending_mode = MODE_SKETCH_PEN;

static struct os_task      g_voice_task_hdl;
static struct os_semaphore g_voice_sema;

/* ========== stream ops (same pattern as AT_play_audio.c) ========== */

static uint32_t voice_get_data_len(void *data)
{
    struct data_structure *d = (struct data_structure *)data;
    return (uint32_t)d->priv;
}

static uint32_t voice_set_data_len(void *data, uint32_t len)
{
    struct data_structure *d = (struct data_structure *)data;
    d->priv = (void *)len;
    return len;
}

static const stream_ops_func voice_stream_ops = {
    .get_data_len = voice_get_data_len,
    .set_data_len = voice_set_data_len,
};

/* ========== stream opcode callback (matches AT_play_audio.c architecture) ========== */

static int voice_opcode_func(stream *s, void *priv, int opcode)
{
    static uint8_t *audio_buf = NULL;
    int res = 0;

    switch (opcode) {
    case STREAM_OPEN_ENTER:
        break;

    case STREAM_OPEN_EXIT:
    {
#ifdef PSRAM_HEAP
        audio_buf = (uint8_t *)os_malloc_psram(VOICE_BUF_COUNT * VOICE_BUF_SIZE);
#else
        audio_buf = (uint8_t *)os_malloc(VOICE_BUF_COUNT * VOICE_BUF_SIZE);
#endif
        os_printf("[voice] audio_buf=%X\r\n", (uint32_t)audio_buf);
        if (audio_buf) {
            stream_data_dis_mem(s, VOICE_BUF_COUNT);
        }
        streamSrc_bind_streamDest(s, R_SPEAKER);
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
        data->ops = (stream_ops_func *)&voice_stream_ops;
        data->data = audio_buf + data_num * VOICE_BUF_SIZE;
    }
    break;

    case STREAM_CLOSE_EXIT:
    {
        if (audio_buf) {
#ifdef PSRAM_HEAP
            os_free_psram(audio_buf);
#else
            os_free(audio_buf);
#endif
            audio_buf = NULL;
        }
    }
    break;

    case STREAM_DATA_FREE:
        break;

    case STREAM_RECV_DATA_FINISH:
        break;

    default:
        break;
    }
    return res;
}

/* ========== background playback task ========== */

static void voice_play_task(void *arg)
{
    while (1) {
        /* wait for playback request */
        os_sema_down(&g_voice_sema, -1);

        printer_mode_t mode = g_pending_mode;
        const voice_clip_t *clip = &g_clips[mode];

        if (!clip->wav_data || clip->wav_len == 0) {
            os_printf("[voice] no data for mode %d\r\n", mode);
            continue;
        }

        /* if already playing, abort and wait for the stream to finish */
        if (g_voice_busy) {
            g_voice_abort = 1;
            while (g_voice_busy) {
                os_sleep_ms(10);
            }
        }

        g_voice_busy  = 1;
        g_voice_abort = 0;

        os_printf("[voice] playing: %s (%lu bytes, %dHz)\r\n",
                  clip->name, clip->wav_len, clip->sample_rate);

        /* enable 8002D amplifier */
        voice_pa_enable();
		os_sleep_ms(300);
        /* save current filter type, then set to SOUND_FILE (same as key_tone) */
        int prev_filter = get_audio_dac_set_filter_type();
        audio_dac_set_filter_type(SOUND_FILE);

        /* open source stream */
        stream *s = open_stream_available("voice_play",
                                          VOICE_BUF_COUNT, 0,
                                          voice_opcode_func, NULL);
        if (!s) {
            os_printf("[voice] open stream failed\r\n");
            audio_dac_set_filter_type(prev_filter);
            voice_pa_disable();
            g_voice_busy = 0;
            continue;
        }
        os_printf("[voice] stream opened, prev_filter=%d\r\n", prev_filter);

        /* switch sample rate if needed (via broadcast command to dest stream) */
        int prev_hz = audio_dac_get_samplingrate();
        if (prev_hz != clip->sample_rate) {
            broadcast_cmd_to_destStream(s, SET_CMD_TYPE(CMD_AUDIO_DAC_MODIFY_HZ, clip->sample_rate));
        }

//        audac_enable_play();

        /* calculate raw read chunk size based on bit depth */
        int audio_len;
        if (clip->bits_per_sample == 24) {
            audio_len = VOICE_BUF_SIZE * 8 / 24;  /* align to samples */
            audio_len = audio_len * 3 / 4;         /* align to even samples */
            audio_len *= 4;
        } else if (clip->bits_per_sample == 8) {
            audio_len = VOICE_BUF_SIZE / 2;
        } else {
            audio_len = VOICE_BUF_SIZE;
        }

        /* send PCM data in chunks (skip 44-byte WAV header) */
        const uint8_t *ptr = clip->wav_data + WAV_HEADER_SIZE;
        uint32_t remaining = clip->wav_len - WAV_HEADER_SIZE;

        while (remaining > 0 && !g_voice_abort) {
            struct data_structure *data = get_src_data_f(s);
            if (!data) {
                os_sleep_ms(1);
                continue;
            }

            int16_t *wav_buf = (int16_t *)get_stream_real_data(data);
            int readLen;

            if (remaining < (uint32_t)audio_len) {
                readLen = remaining;
            } else {
                readLen = audio_len;
            }

            os_memcpy(wav_buf, ptr, readLen);

            /* 24-bit -> 16-bit conversion */
            if (clip->bits_per_sample == 24) {
                for (uint32_t i = 0; i < (uint32_t)(readLen / 3); i++) {
                    int32_t temp = (int32_t)(
                        ((uint32_t)(*((uint8_t *)wav_buf + 3 * i)))       |
                        ((uint32_t)(*((uint8_t *)wav_buf + 3 * i + 1)) << 8)  |
                        ((uint32_t)(*((uint8_t *)wav_buf + 3 * i + 2)) << 16)
                    ) & 0x00FFFFFF;
                    wav_buf[i] = (int16_t)(temp >> 8);
                }
                readLen = readLen * 2 / 3;
            }
            /* 8-bit unsigned -> 16-bit signed conversion */
            else if (clip->bits_per_sample == 8) {
                /* copy raw data to upper half first so we don't clobber during conversion */
                os_memcpy(((uint8_t *)wav_buf) + readLen / 2, wav_buf, readLen);
                for (uint32_t i = 0; i < (uint32_t)readLen; i++) {
                    wav_buf[i] = (int16_t)(*(((uint8_t *)wav_buf) + readLen + i) - 128) << 8;
                }
                readLen = readLen * 2;
            }

            /* stereo -> mono downmix */
            if (clip->channels == 2) {
                for (uint32_t i = 0; i < (uint32_t)(readLen / 4); i++) {
                    wav_buf[i] = (wav_buf[2 * i] + wav_buf[2 * i + 1]) >> 1;
                }
                readLen = readLen / 2;
            }

            voice_set_data_len(data, readLen);
            data->type = SET_DATA_TYPE(SOUND, SOUND_FILE);
            send_data_to_stream(data);

            ptr += (remaining < (uint32_t)audio_len) ? remaining : (uint32_t)audio_len;
            remaining -= (remaining < (uint32_t)audio_len) ? remaining : (uint32_t)audio_len;
        }

        /* wait for DAC to finish playing (same as key_tone) */
        os_sleep_ms(50);

        /* cleanup: restore DAC state before closing stream to avoid race */
        audio_dac_set_filter_type(prev_filter);
        if (prev_hz != audio_dac_get_samplingrate()) {
            audio_da_recfg(prev_hz);
        }
        close_stream(s);
        os_sleep_ms(150);  /* wait for stream GC timer to complete before reusing name */

        /* disable amplifier to save power */
        voice_pa_disable();

        g_voice_busy = 0;
        os_printf("[voice] done: %s\r\n", clip->name);
    }
}

/* ========== public API ========== */

void printer_voice_init(void)
{
    /* fill in wav_len at runtime (linker-time constants, not compile-time) */
    g_clips[MODE_SKETCH_PEN].wav_len = jianbihua_wav_len;
    g_clips[MODE_LINE_ART].wav_len   = xiangao_wav_len;
    g_clips[MODE_PENCIL].wav_len     = sumiao_wav_len;
    g_clips[MODE_ANIME].wav_len      = dongman_wav_len;
    g_clips[MODE_CARTOON].wav_len    = katong_wav_len;
    g_clips[MODE_INK_WASH].wav_len   = shuimohua_wav_len;

    os_sema_init(&g_voice_sema, 0);
    OS_TASK_INIT("voice_play", &g_voice_task_hdl,
                 voice_play_task, NULL,
                 VOICE_TASK_PRIO, VOICE_TASK_STACK);
    os_printf("[voice] init done\r\n");
}

static const char * const tts_mode_names[MODE_COUNT] = {
    "简笔画", "线稿", "素描", "动漫", "卡通", "水墨画"
};

static void tts_announce_task(void *arg)
{
    char *text = (char *)arg;
    extern void baidu_chat_agent_engine_send_text_to_TTS(void *engine, const char *text);
    extern void *g_engine;

    os_printf("[voice] TTS announce: %s\r\n", text);
    baidu_chat_agent_engine_send_text_to_TTS(g_engine, text);
    os_free(text);
}

void printer_voice_announce(printer_mode_t mode)
{
    if (mode >= MODE_COUNT) return;

    extern int is_brtc_running(void);

    if (is_brtc_running()) {
        char *tts_text = (char *)os_malloc(64);
        if (tts_text) {
            snprintf(tts_text, 64, "已切换到%s风格", tts_mode_names[mode]);
            os_task_create("tts_announce", tts_announce_task, tts_text,
                           OS_TASK_PRIORITY_NORMAL, 0, NULL, 20 * 1024);
        }
    } else {
        /* Engine not ready yet (early boot), fallback to local WAV */
        if (g_voice_busy) {
            g_voice_abort = 1;
        }
        g_pending_mode = mode;
        os_sema_up(&g_voice_sema);
    }
}

void printer_voice_wait_done(void)
{
    while (g_voice_busy) {
        os_sleep_ms(10);
    }
}

void printer_voice_deinit(void)
{
    g_voice_abort = 1;
    printer_voice_wait_done();
}
