#include "sys_config.h"
#include "typesdef.h"
#include "list.h"
#include "osal/task.h"
#include "custom_mem/custom_mem.h"
#include "curl/curl.h"
#include "cJSON.h"

#include "../include/baidu_chat_agents_engine.h"
#include "../include/baidu_rtc_client.h"
#include "txw81x_voice.h"
#include "txw81x_video.h"
#include "video_raw_data.h"

// BRTC 客户对接环境访问地址
#define JSON_CONFIG_TEMPLATE "{\"app_id\": \"%s\", \"config\" : \"{\\\"llm\\\" : \\\"%s\\\", \\\"llm_token\\\" : \\\"no\\\", \\\"rtc_ac\\\": \\\"pcmu\\\", \\\"lang\\\" : \\\"%s\\\"}\", \"quick_start\": true}"
#define JSON_CONFIG_TEMPLATE_VISUAL "{\"app_id\": \"%s\", \"config\" : \"{\\\"llm\\\" : \\\"%s\\\", \\\"llm_token\\\" : \\\"no\\\", \\\"enable_visual\\\" : \\\"true\\\", \\\"rtc_ac\\\": \\\"pcmu\\\", \\\"lang\\\" : \\\"%s\\\"}\", \"quick_start\": true}"
#define MAX_APPID_LEN 64
#define MAX_ROOMNAME_LEN 128
#define MAX_PARAM_LENGTH 2048
#define HTTP_TIMEOUT_MS 10000
#define DEFAULT_BRTC_LLM "LLMRacing"  // 内置默认模型， VOLC/XUNFEI/BAIDU 通过竞速命中
#define DEFAULT_BRTC_LANG "zh"        // 默认语言
#define BRTC_LOG os_printf

static bool brtc_running = false;
static uint64_t time_last_asr = 0;
static int is_first_audio = 1;
uint8_t curl_initialized = 0;

/* 使用全局引擎变量，方便各任务访问 */
static BaiduChatAgentEngine *g_engine = NULL;
struct ausrc_st *ausrc;  
struct auplay_st *auplay;  
static bool g_brtc_license_abnormal_exit = false;    //true:表示鉴权失败退出

static bool g_enable_internal_audio = true;         // true: 音频内部采集、播放（默认） false:音频外部采集、播放 （用户需对音频二次处理时使用）
static bool g_enable_local_agent = false;            // true：SDK内部请求客户对接环境创建智能体（仅测试） false: 业务侧请求应用服务（AppServer）创建智能体
static bool g_enable_visual = false;                 // true: 视频模式; false: 普通语音交互模式（默认）
static bool g_vision_mode = VISION_MODE_STREAM;      // VISION_MODE_IMAGE :视觉理解图片模式; VISION_MODE_STREAM 视觉视频流模式 (默认)
static bool g_enable_enhance_query = false;          // 用户query增强 （谨慎使用）
typedef struct {
    char content[MAX_PARAM_LENGTH];      // 响应内容缓冲区
    char ai_agent_instance_id[MAX_ROOMNAME_LEN];
    size_t size;        // 响应内容长度
    long http_code;     // HTTP状态码
    int error;          // 错误码(0表示成功)
} ResponseData;
ResponseData call_resp;

#define VIDEO_NAL_DATA jpeg_640x480_frame
struct photo_st *video_src;  

static bool volatile stop_video_send_flag = false;
void auto_send_video(void *h);

void onErrorCallback(int errCode, const char *errMsg)
{
    BRTC_LOG("Error occurred. Code: %d, Message: %s\n", errCode, errMsg);
}

void onCallStateChangeCallback(AGentCallState state)
{
    BRTC_LOG("Call state changed: %d\n", state);
}

void onConnectionStateChangeCallback(AGentConnectState state)
{
    BRTC_LOG("Connection state changed: %d\n", state);
}

void onUserAsrSubtitleCallback(const char *text, bool isFinal)
{
    BRTC_LOG("User ASR subtitle: %s (isFinal: %d)\n", text, isFinal);
}

void onFunctionCall(const char *id, const char *params)
{
    BRTC_LOG("onFunctionCall, id: %s, params: %s\n", id, params);
}

void jpeg_read_handler(const uint8_t* data, size_t len, void *arg) {
    BRTC_LOG("Read jpeg data len: %d\n", len); 
    if (stop_video_send_flag) {
        return;
    } 
    if (g_engine && len > 0) {
        baidu_chat_agent_engine_send_video(g_engine, (const uint8_t*)data, len);
    }
}

void onMediaSetup(void)
{
    BRTC_LOG("Media setup completed.\n");

    baidu_chat_agent_engine_send_text_to_TTS(g_engine, "你好，我来了");

    // 设置增强Query, 如需要
    if (g_enable_enhance_query) {
        baidu_chat_agent_engine_set_enhance_query(g_engine, 3, "我现在在成都", "用2个字回答问题");
    }

    if(g_enable_visual) {
        if  (g_vision_mode == VISION_MODE_STREAM) {
            baidu_chat_agent_engine_update_visual_mode(g_engine, VISION_MODE_STREAM);

            // 若开启视频视觉理解， 可在此处开始周期性（1000ms一次）采集发送JPEG图片
            // auto_send_video(NULL);
            video_txw81_jpeg_alloc(&video_src, jpeg_read_handler, NULL);
        } else {
            baidu_chat_agent_engine_update_visual_mode(g_engine, VISION_MODE_IMAGE);
        }
    }


}

void onAIAgentSubtitle(const char* text, int len) {
    BRTC_LOG("onAIAgentSubtitle.%s, len:%d\n", text, len);
    time_last_asr = os_jiffies();
}

void onAIAgentSpeaking(bool speeking) {
    BRTC_LOG("onAIAgentSpeaking.\n");
    if (speeking)
        is_first_audio = 1;
}

void onAudioPlayerOp(const char* path, bool start) {
    BRTC_LOG("onAudioPlayerOp. path:%s, start:%d\n", path, start);
}

void onAudioData(const uint8_t *data, size_t len)
{
    // BRTC_LOG("Received audio data of length: %d\n", len);  
    if (auplay && auplay->wh) {
        auplay->wh((void *)data, len, auplay);
    }
}

void onVideoData(const uint8_t *data, size_t len, int width, int height)
{
    BRTC_LOG("Received video data of length: %zu, width: %d, height: %d\n", len, width, height);
}

void onLicenseResult (bool result) {
    printf("onLicenseResult: %d\n", result);
    if (!result) {
        BRTC_LOG("onLicenseResult: failed\n");
        g_brtc_license_abnormal_exit = true;
    }
}

/**
 * @brief 模型识别到视觉意图后没用户请求一张图片，用户可从即时从相机采集一张图片上传。
 *  当然用户也可以先上传图片，再进行询问“图片中能看到什么”
 * 
 */
void onVisionImageRequest() {
    printf("onVisionImageRequest send a image to vision llm ...\n" );
    if  (g_vision_mode == VISION_MODE_IMAGE) {
        baidu_chat_agent_engine_send_video(g_engine, VIDEO_NAL_DATA, sizeof(VIDEO_NAL_DATA));
    }
}

/**
 * @brief 视觉图片上传完成回调
 * 
 * @param name 上传的文件名，可忽略
 */
void onVisionImageAck(const char* name) {
    printf("onVisionImageAck file: %s ...\n", name);
}

void ausrc_read_handler(const void *sampv, size_t sampc, void *arg) {
    // BRTC_LOG("Read audio data len: %d\n", sampc);  
    if (g_engine && sampc > 0) {
        baidu_chat_agent_engine_send_audio(g_engine, (const uint8_t*)sampv, sampc);
    }
}

void setUserParameters(AgentEngineParams *params)
{
    strncpy(params->agent_platform_url, SERVER_HOST_ONLINE, sizeof(params->agent_platform_url) - 1);
    strncpy(params->appid, BDCloudDefaultRTCAppID, sizeof(params->appid) - 1); // //需要和服务端使用同一个appId
    snprintf(params->userId, sizeof(params->userId), "%s", "12345678"); // 终端用户唯一的id号，例如手机号
    strncpy(params->cer, "./a.cer", sizeof(params->cer) - 1);
    strncpy(params->workflow, "VoiceChat", sizeof(params->workflow) - 1);
    snprintf(params->license_key, sizeof(params->license_key), "%s", "292fc11a00ca42daa1101c6987c14b76");  //"xxxx"为license_key字符串，需要购买获得

    params->instance_id = 10373;
    params->verbose = true;
    params->enable_internal_device = g_enable_internal_audio;
    params->enable_local_agent = g_enable_local_agent;
    if(!g_enable_local_agent && call_resp.error == 0 && call_resp.http_code == 200) {
        /** 建议将 baidu AIInteractionServer 返回的json content 透传至sdk， sdk 内部解析, 格式如：
        "{"
        "\"ai_agent_instance_id\": 2230595646193664,"
        "\"context\": {"
        "\"cid\": 1,"
        "\"token\":
        "\"00415f2bd1c3fe5dbc02cc49af7xxxx\"
        "}"
        "}";
        */
        snprintf(params->remote_params, sizeof(params->remote_params), "%s", call_resp.content);
    }
    params->enable_voice_interrupt = true;
    params->level_voice_interrupt = 80;
    strncpy(params->llm, DEFAULT_BRTC_LLM, sizeof(params->llm) - 1);
    strncpy(params->lang, DEFAULT_BRTC_LANG, sizeof(params->lang) - 1);
    params->AudioInChannel = 1;
    params->AudioInFrequency = 8000;
    if(g_enable_visual) {
        params->enable_video = true; // 视觉理解场景开启视频
    }
}

void parse_agent_instance_json(const char *json_string) {
    cJSON *root = cJSON_Parse(json_string);
    if (!root) {
        BRTC_LOG("json parse error: %s\n", cJSON_GetErrorPtr());
        return;
    }

    cJSON *ai_instance_id = cJSON_GetObjectItem(root, "ai_agent_instance_id");
    if (cJSON_IsNumber(ai_instance_id)) {
        long long id_value = (long long)ai_instance_id->valuedouble;
        snprintf(call_resp.ai_agent_instance_id, sizeof(call_resp.ai_agent_instance_id), 
                    "%lld", id_value);
        BRTC_LOG("Parsed agent install id: %s\n", call_resp.ai_agent_instance_id);
    }

    cJSON_Delete(root);
}


// 回调函数处理响应数据
size_t http_callback(void *contents, size_t size, size_t nmemb, void *userdata) {
    ResponseData *resp = (ResponseData *)userdata;
    size_t total_size = size * nmemb;
    BRTC_LOG("Received http content %d bytes:\n", total_size);
    if  (total_size + 1 > MAX_PARAM_LENGTH) {
        return 0;
    }
    memcpy(&(resp->content[resp->size]), contents, total_size);
    resp->size += total_size;
    resp->content[resp->size] = '\0';
    BRTC_LOG("%.*s\n", (int)total_size, resp->content);
    return total_size;
}

int http_post(const char *url, const char *post_data, ResponseData *resp) {
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    int err = 0;
    long http_code = -1;

    memset(resp, 0, sizeof(ResponseData));

    if (!curl_initialized) {
		res = curl_global_init(CURL_GLOBAL_DEFAULT);
		if (res) {
            err = -2;
            return err;
        }
		curl_initialized = 1;
	}

    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(post_data));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, HTTP_TIMEOUT_MS);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
        
        res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            resp->error = res;
            BRTC_LOG("curl_easy_perform() failed[%d]: %s\n", res, curl_easy_strerror(res));
        } else {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->http_code);
        }

        BRTC_LOG("http_post[%s]: status code %d\n", url, resp->http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return resp->error;
    }

    return resp->error;
}

void http_cleanup() {
	if (curl_initialized) {
		curl_global_cleanup();
		curl_initialized = 0;
	}
}

void brtc_demo_init(void)
{
    BaiduChatAgentEvent events = {
        .onError = onErrorCallback,
        .onCallStateChange = onCallStateChangeCallback,
        .onConnectionStateChange = onConnectionStateChangeCallback,
        .onUserAsrSubtitle = onUserAsrSubtitleCallback,
        .onFunctionCall = onFunctionCall,
        .onMediaSetup = onMediaSetup,
        .onAIAgentSubtitle = onAIAgentSubtitle,
        .onAIAgentSpeaking = onAIAgentSpeaking,
        .onAudioPlayerOp = onAudioPlayerOp,
        .onAudioData = onAudioData,
        .onVideoData = onVideoData,
        .onLicenseResult = onLicenseResult,
        .onVisionImageRequest = onVisionImageRequest,
        .onVisionImageAck = onVisionImageAck,
    };
    BRTC_LOG("BRTC AI Engine initialization start with local agent: %d internal audio: %d \n",
            g_enable_local_agent, g_enable_internal_audio);
    BaiduChatAgentEngine *engine = baidu_create_chat_agent_engine(&events);
    if (!engine)
    {
        BRTC_LOG("Engine initialization failed.\r\n");
        return;
    }
    g_engine = engine; // 保存全局变量供后续使用

    AgentEngineParams agentParams;
    memset(&agentParams, 0, sizeof(agentParams));
    setUserParameters(&agentParams);

    int result = baidu_chat_agent_engine_init(engine, &agentParams);
    if (result != 200)
    {
        BRTC_LOG("Failed to log in. Error code: %d\r\n", result);
        baidu_chat_agent_engine_destroy(engine);
        g_engine = NULL;
        return;
    }
    baidu_chat_agent_engine_call(engine);

    // 如开启外部音频，则启动音频采集、播放任务
    if  (!g_enable_internal_audio) {
        struct ausrc_prm prm;
        prm.ch = 1;
        prm.fmt = 0;
        prm.srate = 8000;
        prm.ptime = 20;
        BRTC_LOG("Start external audio task ...\n");
        voice_txw81_src_alloc(&ausrc, &prm, ausrc_read_handler, NULL);
        voice_txw81_play_alloc(&auplay, &prm, ausrc_write_handler, NULL);
    }
    stop_video_send_flag = false;
    brtc_running = true;
    return;
}

// 视觉接口验证，循环发送同一帧 JPEG 静态数据
void auto_send_video(void *h)
{
    if (stop_video_send_flag) {
        return;
    }
    BRTC_LOG("send video nal ...\n");
    baidu_chat_agent_engine_send_video(g_engine, VIDEO_NAL_DATA, sizeof(VIDEO_NAL_DATA));
    brtc_sdk_do_async(auto_send_video, NULL, 1000);
}

void baidu_chat_agent_demo_close(void)
{
    os_printf("baidu_chat_agent_demo_close\n");
    if (g_engine)
    {
        stop_video_send_flag = true;
        if(g_enable_visual && g_vision_mode == VISION_MODE_STREAM) {
            txw81_jpeg_destructor(video_src);
        }
        if (!g_enable_internal_audio) {
            txw81_src_destructor(ausrc);
            txw81_play_destructor(auplay);
        }

        baidu_chat_agent_engine_destroy(g_engine);
        g_engine = NULL;

        if (!g_enable_local_agent && g_engine) {
            sendStopAIAgentInstance();
            http_cleanup();
        }
        brtc_running = false;
    }
}

int sendGenerateAIAgentCall() {
    char config_data[MAX_PARAM_LENGTH] = {0};
    if (g_enable_visual) {
        snprintf(config_data, sizeof(config_data), JSON_CONFIG_TEMPLATE_VISUAL, 
                BDCloudDefaultRTCAppID, DEFAULT_BRTC_LLM, DEFAULT_BRTC_LANG);
    } else {
        snprintf(config_data, sizeof(config_data), JSON_CONFIG_TEMPLATE, 
                BDCloudDefaultRTCAppID, DEFAULT_BRTC_LLM, DEFAULT_BRTC_LANG);
    }

    char request_url[MAX_PARAM_LENGTH] = {0};
    snprintf(request_url, sizeof(request_url), "%s/generateAIAgentCall", SERVER_HOST_ONLINE);
    return http_post(request_url, config_data, &call_resp);
}

int sendStopAIAgentInstance() {
    if (!g_engine) {
        return -1;
    }
    char post_data[MAX_PARAM_LENGTH];
    snprintf(post_data, sizeof(post_data),
         "{\"app_id\":\"%.*s\",\"ai_agent_instance_id\":\"%.*s\"}",
         MAX_APPID_LEN, BDCloudDefaultRTCAppID,
         MAX_ROOMNAME_LEN, call_resp.ai_agent_instance_id);
    char request_url[MAX_PARAM_LENGTH] = {0};
    snprintf(request_url, sizeof(request_url), "%s/stopAIAgentInstance", SERVER_HOST_ONLINE);
    ResponseData stop_resp;
    return http_post(request_url, post_data, &stop_resp);
}

void brtc_demo_start(void *arg)
{
    if (g_enable_local_agent) {
         brtc_demo_init();
    } else {
        int error_code = sendGenerateAIAgentCall();
        if(error_code == 0 && call_resp.http_code == 200) {
            parse_agent_instance_json(call_resp.content);
            brtc_demo_init();
        }
    }
}


struct os_task task_brtc_demo;
void app_main(void)
{
    if (brtc_running){
        os_task_create("brtc_ai_agent_task_close", baidu_chat_agent_demo_close, (void*)NULL, OS_TASK_PRIORITY_NORMAL, 0, NULL, 16 * 1024);
        os_sleep_ms(1000);
    }
    
    if (brtc_running == false)
    {
        OS_TASK_INIT("brtc_demo_start", &task_brtc_demo, brtc_demo_start, NULL, OS_TASK_PRIORITY_BELOW_NORMAL, 1024 * 16);
        os_printf("start  brtc_demo_start task ...\r\n");
    }
}
