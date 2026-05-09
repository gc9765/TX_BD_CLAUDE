#ifndef _LLM_API_H_
#define _LLM_API_H_

#include "basic_include.h"
#include "lib/net/llm/llm_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

//STUB
void *llm_malloc(uint32 size);
void  llm_free(void *ptr);
void *llm_zalloc(size_t size);
void *llm_calloc(size_t nmemb, size_t size);
void *llm_realloc(void *ptr, size_t size);
char *llm_strdup(const char *s);

#define LLM_SESSION_MAX (3)

struct llm_data;
struct llm_model_data;

typedef int32(*llm_evt_cb)(void *session, uint16 evt, uint32 param1, uint32 param2);

enum llm_event {
    LLM_EVENT_UNKNOWN               = 0,
    //general
    LLM_EVENT_STT_RESULT            = 1,
    LLM_EVENT_TTS_RESULT            = 2,
    LLM_EVENT_CHAT_RESULT           = 3,
    LLM_EVENT_ERROR_MSG             = 4,   // 服务器返回错误信息
    LLM_EVENT_FUNC_CALL             = 5,   // Function_calling
    LLM_EVENT_TIMEOUT               = 6,   // 超时
    LLM_EVENT_CUSTOMIZE             = 7,   // 自定义（内部未知事件）

    //llm_tts
    LLM_EVENT_TTS_AUDIO_START       = 8,
    LLM_EVENT_TTS_AUDIO_ERR         = 9,

    //llm_chat
    LLM_EVENT_CHAT_CONTEXT_ID       = 10,
    
    //llm_sts
    LLM_EVENT_CONNECTED             = 11,   // 已连接
    LLM_EVENT_DISCONNECT            = 12,   // 已断开
    LLM_EVENT_VAD_RESULT            = 13,   // VAD 结果
    LLM_EVENT_DIALOGUE_START        = 14,   // 对话开始
    LLM_EVENT_DIALOGUE_TIMEOUT      = 15,   // 对话结束
    LLM_EVENT_DIALOGUE_END          = 16,   // 对话超时
    LLM_EVENT_INTERRUPT_END         = 17,   // 打断结束
    LLM_EVENT_WAITING_END           = 18,   // 等待通知服务器结束
    LLM_EVENT_UPLOAD_FILE_RESULT    = 19,   // 上传文件结果

    //llm_tti
    LLM_EVENT_TTI_RESULT            = 20,

    LLM_EVENT_TX_ERR                = 21,
    LLM_EVENT_RX_ERR                = 22,
};

typedef enum {
    LLM_CONFIG_TYPE_MODEL,
    LLM_CONFIG_TYPE_TRANS,
} llm_cfg_type;

typedef enum {
    LLM_CTRL_TYPE_RUN           = 0,
    LLM_CTRL_TYPE_STOP          = 1,
    LLM_CTRL_TYPE_INTERRUPT     = 2,
    LLM_CTRL_TYPE_RECONNECT     = 3,
    LLM_CTRL_TYPE_WAITING       = 4,
} llm_ctrl_type;

typedef enum {
    LLM_DATA_STATE_START        = 1,
    LLM_DATA_STATE_MIDDLE       = 2,
    LLM_DATA_STATE_END          = 3,
} llm_data_state;

typedef enum {
    LLM_DATA_TYPE_MGMT          = 0,
    LLM_DATA_TYPE_AUDIO         = 1,
    LLM_DATA_TYPE_FILE          = 2,
    LLM_DATA_TYPE_TEXT          = 3,
    LLM_DATA_TYPE_RAW           = 4,
    LLM_DATA_TYPE_TTS           = 5,
} llm_data_type;

struct llm_model {
    const char *name;
    const struct llm_model_data *m;
};

struct llm_global_param {
    uint16 task_reuse: 1, rev: 15;
    uint16 model_count;
    const struct llm_model *models;
};

struct llm_trans_param {
    size_t      buffersize;
    size_t      upload_buffersize;
    long        low_speed_limit;
    long        low_speed_time;
    long        connect_timeout;
    size_t      blob_len;
    void        *blob_data;
};

struct llm_chat_sparam {
    uint16 qmsg_tx_cnt;
    uint16 qmsg_rx_cnt;
    uint16 reply_fragment_size;
    uint16 context_id_len;
    void   *evt_cb;
};

struct llm_stt_sparam {
    uint16 qmsg_tx_cnt;
    uint16 cb_max_size;
    void   *evt_cb;
};

struct llm_tts_sparam {
    uint16 qmsg_tx_cnt;
    uint16 rx_buff_size;
    uint16 cb_max_size;
    void   *evt_cb;
};

struct llm_tti_sparam {
    uint16 qmsg_tx_cnt;
    uint16 cb_max_size;
    void   *evt_cb;
};

struct llm_sts_sparam {
    uint16 audio_url_cnt;
    uint16 qmsg_tx_cnt;
    uint16 rx_buff_size;
    uint16 cb_max_size;
    uint16 retry_cnt;
    void   *evt_cb;
};

int32 llm_global_init(struct llm_global_param *global);
int32 llm_global_deinit(void);

//CHAT
//void *llm_chat_init(char *llm_name, struct llm_chat_sparam *sparam);
//int32 llm_chat_deinit(void *chat_hdl);
//int32 llm_chat_send(void *chat_hdl, char *question, uint32 question_len);
//int32 llm_chat_recv(void *chat_hdl, char *buff, uint32 buff_size);
//int32 llm_chat_set_role(void *chat_hdl, char *role);
//int32 llm_chat_set_prefill(void *chat_hdl, char *prefill);
//int32 llm_chat_change_dialogue(void *chat_hdl, char *context_id, uint32 content_len);
//int32 llm_chat_new_dialogue(void *chat_hdl, char *cache_mode);
//int32 llm_chat_config(void *chat_hdl, llm_cfg_type type, void *cfg, uint32 cfg_size);
//int32 llm_chat_stop(void *chat_hdl);

//STT
void *llm_stt_init(char *llm_name, struct llm_stt_sparam *sparam);
int32 llm_stt_deinit(void *stt_hdl);
int32 llm_stt_send(void *stt_hdl, llm_data_type type, llm_data_state state, char *audio, uint32 audio_len);
int32 llm_stt_recv(void *stt_hdl, char *buff, uint32 buff_size);
int32 llm_stt_config(void *stt_hdl, llm_cfg_type type, void *cfg, uint32 cfg_size);
int32 llm_stt_ctrl(void *stt_hdl, llm_ctrl_type ctrl_type, uint32 timeout);

#define llm_stt_interrupt(stt_hdl, timeout) llm_stt_ctrl(stt_hdl, LLM_CTRL_TYPE_INTERRUPT, timeout)

//TTS
//void *llm_tts_init(char *llm_name, struct llm_tts_sparam *sparam);
//int32 llm_tts_deinit(void *tts_hdl);
//int32 llm_tts_send(void *tts_hdl, char *text, uint32 text_len, llm_data_state state);
//int32 llm_tts_recv(void *tts_hdl, char *buff, uint32 buff_size);
//int32 llm_tts_config(void *tts_hdl, llm_cfg_type type, void *cfg, uint32 cfg_size);
//int32 llm_tts_stop(void *tts_hdl);
//int32 llm_tts_play_audio(void *tts_hdl, const       char *audio, uint32 length);

//TTI
void *llm_tti_init(char *llm_name, struct llm_tti_sparam *sparam);
int32 llm_tti_deinit(void *tti_hdl);
int32 llm_tti_send(void *tti_hdl, llm_data_type type, llm_data_state state, char *prompt, uint32 prompt_len);
int32 llm_tti_recv(void *tti_hdl, char *buff, uint32 buff_size);
int32 llm_tti_config(void *tti_hdl, llm_cfg_type type, void *cfg, uint32 cfg_size);
int32 llm_tti_ctrl(void *tti_hdl, llm_ctrl_type ctrl_type, uint32 timeout);

#define llm_tti_interrupt(tti_hdl, timeout) llm_tti_ctrl(tti_hdl, LLM_CTRL_TYPE_INTERRUPT, timeout)

//STS
void *llm_sts_init(char *llm_name, struct llm_sts_sparam *sparam);
int32 llm_sts_deinit(void *sts_hdl);
int32 llm_sts_play_audio(void *sts_hdl, const char *audio, uint32 length);
int32 llm_sts_check_audio(void *sts_hdl);
int32 llm_sts_send(void *sts_hdl, llm_data_type type, llm_data_state state, char *audio, uint32 audio_len);
int32 llm_sts_recv(void *sts_hdl, char *buff, uint32 buff_size);
int32 llm_sts_config(void *sts_hdl, llm_cfg_type type, void *cfg, uint32 cfg_size);
int32 llm_sts_ctrl(void *sts_hdl, llm_ctrl_type ctrl_type, uint32 timeout);
int32 llm_sts_upload_file(void *sts_hdl, llm_data_type type, llm_data_state state, 
                                char *data, uint32 data_len);

#define llm_sts_run(sts_hdl)                llm_sts_ctrl(sts_hdl, LLM_CTRL_TYPE_RUN, 0)
#define llm_sts_stop(sts_hdl)               llm_sts_ctrl(sts_hdl, LLM_CTRL_TYPE_STOP, 0)
#define llm_sts_reconnect(sts_hdl)          llm_sts_ctrl(sts_hdl, LLM_CTRL_TYPE_RECONNECT, 0)
#define llm_sts_interrupt(sts_hdl, timeout) llm_sts_ctrl(sts_hdl, LLM_CTRL_TYPE_INTERRUPT, timeout)
#define llm_sts_wait(sts_hdl, timeout)      llm_sts_ctrl(sts_hdl, LLM_CTRL_TYPE_WAITING, timeout)

/////////////////////////////////////////////////////////////////////
//doubao
//#include "lib/net/llm/doubao/doubao_llm_chat.h"
#include "lib/net/llm/doubao/doubao_asr_stt.h"
//#include "lib/net/llm/doubao/doubao_asr_tts.h"
#include "lib/net/llm/doubao/doubao_llm_tti.h"

//listenai
//#include "lib/net/llm/listenai/listenai.h"

//coze
#include "lib/net/llm/coze/coze.h"

/////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif

#endif

