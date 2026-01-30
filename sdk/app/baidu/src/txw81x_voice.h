#ifndef TXW81X_VOICE_H__
#define TXW81X_VOICE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "osal/string.h"
#include "custom_mem/custom_mem.h"
#include "stream_frame.h"
#include "osal/task.h"
#include "osal_file.h"

struct ausrc_prm {
	uint32_t   srate;       /**< Sampling rate in [Hz]      */
	uint8_t    ch;          /**< Number of channels         */
	uint32_t   ptime;       /**< Wanted packet-time in [ms] */
	int        fmt;         /**< Sample format (enum aufmt) */
};

typedef void (ausrc_read_h)(const void *sampv, size_t sampc, void *arg);
typedef void (auplay_write_h)(void *sampv, size_t sampc, void *arg);
typedef void (ausrc_error_h)(int err, const char *str, void *arg);

struct ausrc_st {
    const struct ausrc *as; /* inheritance */
    struct aubuf *aubuf;
    uint32_t srate; /**< Sampling rate in [Hz]*/
    size_t sampsz;
    void *sampv;
    size_t sampc;
    ausrc_read_h *rh;
    void *arg;
    volatile bool ready;
    struct os_task task;
    uint8_t filename_prefix[4];
};

struct auplay_st {
    const struct auplay *ap; /* inheritance */
    struct aubuf *aubuf;
    uint32_t srate; /**< Sampling rate in [Hz]*/
    size_t sampsz;
    void *sampv;
    size_t sampc;
    auplay_write_h *wh;
    void *arg;
    volatile bool ready;
    struct os_task at_play_audio_task;
};

int voice_txw81_src_alloc(
        struct ausrc_st **stp,
        struct ausrc_prm *prm,
        ausrc_read_h *rh,
        void *arg);

int voice_txw81_play_alloc(
        struct auplay_st **stp,
        struct ausrc_prm *prm,
        auplay_write_h *wh,
        void *arg);

void ausrc_write_handler(void *sampv, size_t sampc, void *arg);

#ifdef __cplusplus
}
#endif


#endif /* TXW81X_VOICE_H__ */