#ifndef TXW81X_VOICE_H__
#define TXW81X_VOICE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "osal/string.h"
#include "stream_frame.h"
#include "osal/task.h"

typedef  void (jpeg_read_h)(const uint8_t* data, size_t len, void *arg);
struct photo_st
{
    struct os_task task;
    jpeg_read_h *rh;
    uint32_t photo_num;
    uint8_t filename_prefix[4];
    uint8_t running;
};

void txw81_jpeg_destructor(void *arg);
int video_txw81_jpeg_alloc( struct photo_st **stp, jpeg_read_h * rh, void *arg);

#ifdef __cplusplus
}
#endif


#endif /* TXW81X_VOICE_H__ */