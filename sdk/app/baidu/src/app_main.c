#include "sys_config.h"
#include "typesdef.h"
#include "list.h"
#include "osal/task.h"
#include "osal_file.h"
#include "custom_mem/custom_mem.h"
//#include "lwip/sockets.h"
#include "curl/curl.h"
#include "cJSON.h"

#include "../include/baidu_chat_agents_engine.h"
#include "../include/baidu_rtc_client.h"
#include "playback/playback.h"
#include "txw81x_voice.h"
#include "txw81x_video.h"
#include "video_raw_data.h"
#include "lib/lcd/lcd.h"


// 客户对接环境 (仅接入验证,不可用于生产环境)
#define SERVER_HOST_BD_DEV              "http://ai.agent.kaywang.cn:8988/api/v1/aiagent"
#define SERVER_HOST_DEV_AMERICA         "线下提供"
// 上线时须切换为用户自已的app server地址
#define SERVER_HOST_USE_ONLINE              "user_online_app_server_url" // 用户生产环境地址
//文生图使用irag模型
#define BDCloudRTCAppID          "apppibr915pu75m" // REGION_BD_DEV APPID
// 支持云音乐播放、图生图使用imageprocess
// #define BDCloudRTCAppID          "appqb1g7txx1k8q"
// #define BDCloudRTCAppID          "appmdty71uuwx8u"       // REGION_AMERICA APPID

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
//#define DUMP_VIDEO_FRAME
#define BW_LINE_DRAWING

static bool brtc_running = false;
static uint64_t time_last_asr = 0;
int is_first_audio = 0;
uint8_t curl_initialized = 0;

/* 使用全局引擎变量，方便各任务访问 */
static BaiduChatAgentEngine *g_engine = NULL;
struct ausrc_st *ausrc;  
struct auplay_st *auplay;  
static bool g_brtc_license_abnormal_exit = false;    //true:表示鉴权失败退出

static bool g_enable_internal_audio = false;         // true: 音频内部采集、播放（默认） false:音频外部采集、播放 （用户需对音频二次处理时使用）
static bool g_enable_local_agent = false;            // true：SDK内部请求客户对接环境创建智能体（仅测试） false: 业务侧请求应用服务（AppServer）创建智能体
static bool g_enable_visual = true;                 // true: 视觉理解模式（依赖开启视频）， 与图片生成模式互斥； false: 普通语音交互模式（默认）
static bool g_vision_mode = VISION_MODE_IMAGE;       // VISION_MODE_IMAGE :视觉理解图片模式; VISION_MODE_STREAM 视觉视频流模式 (默认)
static bool g_enable_enhance_query = false;          // 用户query增强 （谨慎使用）
static bool g_enable_image_generate = false;          // 开启图片生成模式（默认开启）,依赖开启视频。与视觉理解模式互斥;

static Region region = REGION_BD_DEV;           // 当前除北美外的其它接入点暂未开放，默认使用中国大陆 REGION_MAINLAND

char g_platform_host[256];
char g_appid [MAX_APPID_LEN]="";

// 识图
static const char* object_vision_prompt = 
    "# 你是一位资深的早教老师，每次会收到一张来自幼儿园或小学生的物品照片。请识别照片中的物品并输出它的中文汉字和英文单词名称及如字典一样详细的中英文释义。\\\\n\\\\n"
    "## 中文释义要求：\\\\n"
    "1. 根据<汉字>进行回答\\\\n"
    "2. 回答<汉字>的笔画数量，格式：`strokenumber：{number}`\\\\n"
    "3. 回答<汉字>的偏旁部首，格式：`radical：{word}`\\\\n"
    "4. 按次序提供<汉字>的不同词义，并为每个词义提供1～2个包含该汉字的词语\\\\n"
    "5. 保证内容简洁明了，易于低龄学生理解\\\\n\\\\n"
    "## 英文释义要求：\\\\n"
    "1. 根据<单词>进行回答\\\\n"
    "2. 回答<单词>的美式音标，格式：`soundmark_us：{phonetic}`\\\\n"
    "3. 回答<单词>的英式音标，格式：`soundmark_en：{phonetic}`\\\\n"
    "4. 按次序提供<单词>的不同词性及中文翻译\\\\n"
    "5. 提供1～2句包含<单词>的示例语句\\\\n"
    "6. 保证内容简洁明了，易于低龄学生理解\\\\n\\\\n"
    "## 输出格式示例（如收到树的图片）：\\\\n"
    "树。\\\\n"
    "tree。\\\\n"
    "((CUSTOM:\\\\n"
    "{\\\\n"
    "  \\\\\\\"name_ch\\\\\\\": \\\\\\\"花\\\\\\\",\\\\n"
    "  \\\\\\\"name_en\\\\\\\": \\\\\\\"flower\\\\\\\",\\\\n"
    "  \\\\\\\"comments_ch\\\\\\\": {\\\\n"
    "    \\\\\\\"name\\\\\\\": \\\\\\\"花\\\\\\\",\\\\n"
    "    \\\\\\\"strokenumber\\\\\\\": 7,\\\\n"
    "    \\\\\\\"radical\\\\\\\": \\\\\\\"艹\\\\\\\",\\\\n"
    "    \\\\\\\"comment\\\\\\\": \\\\\\\"1. 种子植物的有性繁殖器官，由花瓣、花萼、花托、花蕊组成。词语：一朵花。\\\\\\\\\\\\n2. 可供观赏的植物。词语：花草。\\\\\\\",\\\\n"
    "    \\\\\\\"example_sentence\\\\\\\": \\\\\\\"学校里种了许许多多五颜六色的花。\\\\\\\"\\\\n"
    "  },\\\\n"
    "  \\\\\\\"comments_en\\\\\\\": {\\\\n"
    "    \\\\\\\"name\\\\\\\": \\\\\\\"flower\\\\\\\",\\\\n"
    "    \\\\\\\"soundmark_us\\\\\\\": \\\\\\\"/ˈflaʊ.ɚ/\\\\\\\",\\\\n"
    "    \\\\\\\"soundmark_en\\\\\\\": \\\\\\\"/ˈflaʊ.ə/\\\\\\\",\\\\n"
    "    \\\\\\\"comment\\\\\\\": \\\\\\\"n. 花；精华；开花植物\\\\\\\\\\\\nv. 开花；繁荣；成熟\\\\\\\",\\\\n"
    "    \\\\\\\"example_sentence\\\\\\\": \\\\\\\"There are many different kinds of flowers in the garden.\\\\\\\"\\\\n"
    "  }\\\\n"
    "}\\\\n"
    "))\\\\n\\\\n"
    "## 请按以下格式应答：\\\\n"
    "<汉字>:{识别出的中文名称}。\\\\n"
    "<单词>:{识别出的英文名称}。\\\\n"
    "((CUSTOM:\\\\n"
    "{\\\\n"
    "  \\\\\\\"name_ch\\\\\\\": \\\\\\\"{中文名称}\\\\\\\",\\\\n"
    "  \\\\\\\"name_en\\\\\\\": \\\\\\\"{英文名称}\\\\\\\",\\\\n"
    "  \\\\\\\"comments_ch\\\\\\\": {\\\\n"
    "    \\\\\\\"name\\\\\\\": \\\\\\\"{中文名称}\\\\\\\",\\\\n"
    "    \\\\\\\"strokenumber\\\\\\\": {笔画数},\\\\n"
    "    \\\\\\\"radical\\\\\\\": \\\\\\\"{偏旁部首}\\\\\\\",\\\\n"
    "    \\\\\\\"comment\\\\\\\": \\\\\\\"{详细释义}\\\\\\\",\\\\n"
    "    \\\\\\\"example_sentence\\\\\\\": \\\\\\\"{示例句子}\\\\\\\"\\\\n"
    "  },\\\\n"
    "  \\\\\\\"comments_en\\\\\\\": {\\\\n"
    "    \\\\\\\"name\\\\\\\": \\\\\\\"{英文名称}\\\\\\\",\\\\n"
    "    \\\\\\\"soundmark_us\\\\\\\": \\\\\\\"{美式音标}\\\\\\\",\\\\n"
    "    \\\\\\\"soundmark_en\\\\\\\": \\\\\\\"{英式音标}\\\\\\\",\\\\n"
    "    \\\\\\\"comment\\\\\\\": \\\\\\\"{词性和释义}\\\\\\\",\\\\n"
    "    \\\\\\\"example_sentence\\\\\\\": \\\\\\\"{示例句子}\\\\\\\"\\\\n"
    "  }\\\\n"
    "}\\\\n"
    "))";

// 儿童打印机 prompt
static char* object_GeneraetImage_prompt =  
	"# 你是一个资深的黑白简笔画创作者，生成图片时请按下面要求生成精致的黑白简笔画。\\\\n"
	"1. 背景要求：纯白背景（#FFFFFF）；\\\\n"
    "2. 主体要求：黑色线条构成，简笔画风格，无其它颜色填充；\\\\n"
	"3. 颜色要求：白底黑线，只用纯黑线（#000000）与纯白背景（#FFFFFF），无灰度、无填色、无阴影、无高光、无纹理、无噪点。\\\\n"
	"4. 线条要求：统一且细的单一线宽（single consistent thin stroke），干净、连续、锐利；\\\\n"
	"5. 细节要求：允许在主体内部加入更多**同线宽的细节线条**（例如：发丝、衣褶、须眉、纹饰等）以增加精致感，但所有细节必须由与轮廓相同的单一线宽构成。\\\\n"
	"6. 总体要求：构图需简洁、留白充分、主体明确。";

static char at_query_text[4096];
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
void agent_update_prompt(void * text);
void update_prompt(const char *prompt_mode);

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
            video_txw81_jpeg_alloc(&video_src, jpeg_read_handler, NULL); //创建拍照任务线程
        } else {
            baidu_chat_agent_engine_update_visual_mode(g_engine, VISION_MODE_IMAGE);
        }
    } else if (g_enable_image_generate) {
        // 如果不使用视觉理解图片模式，则可以开启图片生成
        baidu_chat_agent_engine_send_event_to_agent(g_engine, AGENT_EVENT_ENABLE_MEDIA_GENERATE);
    }
}

void onAIAgentSubtitle(const char* text, int len) {
    BRTC_LOG("onAIAgentSubtitle.%s, len:%d\n", text, len);
    time_last_asr = os_jiffies();
}

void onAIAgentSpeaking(bool speeking) {
    os_printf("=== CALLBACK onAIAgentSpeaking: speeking=%d ===\n", speeking);
    BRTC_LOG("onAIAgentSpeaking: %d\n", speeking);
    if (speeking) {
        is_first_audio = 1;
        os_printf("=== Set is_first_audio = 1 ===\r\n");
		printf("\n=== TTS START ===\r\n");
    } else {
        is_first_audio = 0;
        os_printf("=== Set is_first_audio = 0 ===\r\n");
		printf("\n=== TTS END ===\r\n");
    }
}

void onAudioPlayerOp(const char* path, bool start) {
    BRTC_LOG("onAudioPlayerOp. path:%s, start:%d\n", path, start);
}

void onAudioData(const uint8_t *data, size_t len)
{
//     BRTC_LOG("Received audio data of length: %d\n", len);
    if (auplay && auplay->wh) {
        auplay->wh((void *)data, len, auplay);  //调用 ausrc_write_handler,如何实现调用的？
    }
}

void dumpVideoFrame (uint8* filename, const uint8_t *data, size_t len) 
{
    int w_len = 0;
    int frame_size = 0;
    // char filename[64] = {0};
    void *fp  = NULL;
    BRTC_LOG("Entry %s:%d\n", __FUNCTION__, __LINE__);

//	os_sprintf(filename, "0:video%04d.jpg", (uint32_t)os_jiffies() % 9999);
    BRTC_LOG("video framr dump name:%s\n", filename);
    fp = osal_fopen(filename,"wb+");
    if(!fp)
    {
        BRTC_LOG("Entry %s:%d\n", __FUNCTION__, __LINE__);
        return;
    }
    w_len = osal_fwrite(data, len, 1, fp);

    if(fp)
    {
        osal_fclose(fp);
    }

    BRTC_LOG("%s[%d] write video frame %d/%d \n", __FUNCTION__, __LINE__, w_len, len);
}



void onVideoData(const uint8_t *data, size_t len, RtcImageType imgtype, int width, int height)
{
    BRTC_LOG("FrameReceived video data of length: %d, width: %d, height: %d\n", len, width, height);
    if (imgtype == RTC_IMAGE_TYPE_JPEG) {
        // render to display
		struct lcdc_device *lcd_dev;
		extern uint8 *video_decode_mem;
		extern uint8 *video_decode_mem1;
		extern uint8 *video_decode_mem2;
		extern uint8 video_decode_config_mem[SCALE_PHOTO1_CONFIG_W*PHOTO1_H+SCALE_PHOTO1_CONFIG_W*PHOTO1_H/2];
		lcd_dev = (struct lcdc_device *)dev_get(HG_LCDC_DEVID);	
		lcd_user_frame(video_decode_config_mem);
		jpg_dec_scale_del();
		set_lcd_photo1_config(SCALE_WIDTH,SCALE_HIGH,0);
		jpg_decode_scale_config((uint32)video_decode_mem);
		lcdc_set_video_en(lcd_dev,1);
//#ifdef DUMP_VIDEO_FRAME
//        if (len > 0) {
//            char filename[64] = {0};
//            os_sprintf(filename, "0:/img%04d.jpg", (uint32_t)os_jiffies() % 9999);
//            dumpVideoFrame(filename, data, len);
//            jpeg_photo_explain(filename, 320, 240);
//        }
//#endif
		// 禁用OSD层，显示VIDEO层
		lcdc_set_osd_en(lcd_dev, 0);
        jpeg_photo_renderer(data, len, 320, 240); //
    }
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
		os_printf("[onVisionImageRequest] g_vision_mode == VISION_MODE_IMAGE\r\n");
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

// 通知
void onMediaGenerateResult(const char* result) {
    if (result && strlen(result) > 0) {
        BRTC_LOG("onMediaGenerateResult content: %s\n", result);
    }
}

// 麦克风音频数据处理
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
    snprintf(params->userId, sizeof(params->userId), "%s", "12345678"); // 终端用户唯一的id号，例如手机号/MAC地址
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
    if(g_enable_visual || g_enable_image_generate) {
        params->enable_video = true; // 视觉理解场景开启视频
    }
    params->region = region;
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

//  static int brtc_debug_callback(CURL *handle, curl_infotype type,
//                                 char *data, size_t size, void *userptr)
//  {
//      const char *prefix = "";
//      switch (type) {
//          case CURLINFO_TEXT:
//              prefix = "[CURL*]";
//              break;
//          case CURLINFO_HEADER_IN:
//              prefix = "[CURL < HDR]";
//              break;
//          case CURLINFO_DATA_IN:
//              prefix = "[CURL < DATA]";
//              break;
//          case CURLINFO_HEADER_OUT:
//              prefix = "[CURL > HDR]";
//              break;
//          case CURLINFO_DATA_OUT:
//              prefix = "[CURL > DATA]";
//              break;
//          case CURLINFO_SSL_DATA_IN:
//              prefix = "[CURL < SSL]";
//              break;
//          case CURLINFO_SSL_DATA_OUT:
//              prefix = "[CURL > SSL]";
//              break;
//          default:
//              return 0;
//      }
//
//      os_printf("%s %.*s", prefix, (int)size, data);
//      return 0;
//  }

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
    os_printf("### http_post start!\r\n");
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
		
//		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
//		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, brtc_debug_callback);
        
        res = curl_easy_perform(curl);  // HTTP请求，自动处理DNS、TCP、HTTP
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

void onAgentEventUpdated(const char* event_msg, size_t len) {
	BRTC_LOG("onAgentEventUpdated: %.*s\n", (int)len, event_msg);
}

void brtc_demo_init(void)
{
	// 初始化13个回调函数
    BaiduChatAgentEvent events = {
        .onError = onErrorCallback,                                      	// 错误回调
        .onCallStateChange = onCallStateChangeCallback, 					// 呼叫状态变化回调
        .onConnectionStateChange = onConnectionStateChangeCallback, 		// 连接状态变化回调
        .onUserAsrSubtitle = onUserAsrSubtitleCallback,  					// 用户语音识别结果回调
        .onFunctionCall = onFunctionCall,   								// 功能调用回调
        .onMediaSetup = onMediaSetup,       								// 媒体建立完成回调
        .onAIAgentSubtitle = onAIAgentSubtitle,   							// 智能体字幕回调（是不是可以用于语音交互中断？）
        .onAIAgentSpeaking = onAIAgentSpeaking,   							// 智能体说话回调
        .onAudioPlayerOp = onAudioPlayerOp,       							// 音频播放器操作回调
        .onAudioData = onAudioData,              							// 音频数据回调
        .onVideoData = onVideoData,             							// 视频数据回调
        .onLicenseResult = onLicenseResult,      							// 鉴权结果回调
        .onVisionImageRequest = onVisionImageRequest,  						// 视觉图片请求回调
        .onVisionImageAck = onVisionImageAck,          						// 视觉图片上传完成回调
        .onMediaGenerateResult = onMediaGenerateResult,  					// 媒体生成结果回调
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
        voice_txw81_src_alloc(&ausrc, &prm, ausrc_read_handler, NULL);   // 创建麦克风音频数据流任务
        voice_txw81_play_alloc(&auplay, &prm, ausrc_write_handler, NULL);
    }
    stop_video_send_flag = false;
    brtc_running = true;

    return;
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
	os_printf("### sendGenerateAIAgentCall start \r\n");
	bool multi_modal = g_enable_visual || g_enable_image_generate;
	
	os_printf("###  multi_modal = g_enable_visual || g_enable_image_generate == %d \r\n",multi_modal);
    char config_data[MAX_PARAM_LENGTH] = {0};
    if (g_enable_visual) {
        snprintf(config_data, sizeof(config_data), JSON_CONFIG_TEMPLATE_VISUAL, 
                 g_appid, DEFAULT_BRTC_LLM, DEFAULT_BRTC_LANG);
    } else {
        snprintf(config_data, sizeof(config_data), JSON_CONFIG_TEMPLATE, 
                 g_appid, DEFAULT_BRTC_LLM, DEFAULT_BRTC_LANG);
    }

    char request_url[MAX_PARAM_LENGTH] = {0};
    snprintf(request_url, sizeof(request_url), "%s/generateAIAgentCall", g_platform_host);
    return http_post(request_url, config_data, &call_resp);
}

int sendStopAIAgentInstance() {
	os_printf("### sendStopAIAgentInstance start \r\n");
    if (!g_engine) {
        return -1;
    }
    char post_data[MAX_PARAM_LENGTH];
    snprintf(post_data, sizeof(post_data),
             "{\"app_id\":\"%.*s\",\"ai_agent_instance_id\":\"%.*s\"}",
             MAX_APPID_LEN, g_appid,
             MAX_ROOMNAME_LEN, call_resp.ai_agent_instance_id);
    char request_url[MAX_PARAM_LENGTH] = {0};
    snprintf(request_url, sizeof(request_url), "%s/stopAIAgentInstance", g_platform_host);
    ResponseData stop_resp;
    return http_post(request_url, post_data, &stop_resp);
}

void brtc_demo_start(void *arg)
{
    if (region == REGION_AMERICA) {
        snprintf(g_platform_host, sizeof(g_platform_host), "%s", SERVER_HOST_DEV_AMERICA);
    } else {
        snprintf(g_platform_host, sizeof(g_platform_host), "%s", SERVER_HOST_BD_DEV);
    }

    if (strlen(g_appid) == 0) {
        snprintf(g_appid, sizeof(g_appid), "%s", BDCloudRTCAppID);
		os_printf("### g_appid == %s \r\n",g_appid);
    }

    if (g_enable_local_agent) {
		os_printf("### brtc_demo_init start\r\n");
         brtc_demo_init();
    } else {
		os_printf("### sendGenerateAIAgentCall start.\r\n");
        int error_code = sendGenerateAIAgentCall();
        if(error_code == 0 && call_resp.http_code == 200) {
            parse_agent_instance_json(call_resp.content);
            brtc_demo_init();
        }
    }
	
	#ifdef BW_LINE_DRAWING
		os_sleep_ms(500);
		update_prompt("3");
	#endif
}


struct os_task task_brtc_demo;
void app_main(void)
{
    if (brtc_running){  //作用是什么？
        os_task_create("brtc_ai_agent_task_close", baidu_chat_agent_demo_close, (void*)NULL, OS_TASK_PRIORITY_NORMAL, 0, NULL, 16 * 1024);
        os_sleep_ms(1000);
    }
    
    if (brtc_running == false)
    {
        OS_TASK_INIT("brtc_demo_start", &task_brtc_demo, brtc_demo_start, NULL, OS_TASK_PRIORITY_BELOW_NORMAL, 1024 * 16);
        os_printf("start  brtc_demo_start task ...\r\n");
    }
}

void agent_send_text(void * text) {
    baidu_chat_agent_engine_send_text(g_engine, (char *)text);
}

void send_text(const char *text) {
    if (text && os_strlen(text) > 0) {
        os_memset(at_query_text, 0, sizeof(at_query_text) -1);
        strncpy(at_query_text, text, sizeof(at_query_text) -1);
        os_printf("at query text:%s\r\n", at_query_text);
        os_task_create("brtc_task_send_text", agent_send_text, (void*)at_query_text, OS_TASK_PRIORITY_NORMAL, 0, NULL, 16 * 1024);
    }
}

static int g_send_frame_num = 0;
void send_video_frame() {
    if (stop_video_send_flag || g_send_frame_num-- == 0) {
        return;
    }
    os_printf("%s:%d send_frame_num:%d\n",__FUNCTION__,__LINE__, g_send_frame_num);
    baidu_chat_agent_engine_send_video(g_engine, VIDEO_NAL_DATA, sizeof(VIDEO_NAL_DATA));
    brtc_sdk_do_async(send_video_frame, NULL, 1000);
}

// 视觉接口验证，循环发送同一帧 JPEG 静态数据
void send_video(const char *frame_num)
{
    // 从argv 获取发送帧数
    g_send_frame_num = os_atoi(frame_num);
    os_printf("%s:%d start send_frame_num:%d\n",__FUNCTION__,__LINE__, g_send_frame_num);
    send_video_frame();
}

void agent_update_prompt(void * text) {
	baidu_chat_agent_engine_send_event_to_agent(g_engine, (char *)text);
}

// model_type  2:视觉理解 (其它类型模型prompt更新暂未支持)
static const char g_updata_vision_prompt_cmd[] = "%s{\\\"model_type\\\":\\\"%s\\\",\\\"prompt\\\":\\\"%s\\\"}";
void update_vision_prompt(char *prompt) {
	int prompt_len = prompt ? strlen(prompt) : 0;
	int total_len = prompt_len + strlen(g_updata_vision_prompt_cmd) + 64;
	char *update_prompt_cmd = (char *)custom_malloc_psram(total_len);

	if (update_prompt_cmd) {
		snprintf(update_prompt_cmd, total_len, g_updata_vision_prompt_cmd,
					AGENT_EVENT_UPDATE_SYSTEM_PROMPT, "3", prompt);
	} else {
		os_printf("update_vision_prompt malloc failed\r\n");
		return;
	}
	strncpy(at_query_text, update_prompt_cmd, sizeof(at_query_text) -1);
	custom_free_psram(update_prompt_cmd);
}

void update_prompt(const char *prompt_mode) {
    int mode = os_atoi(prompt_mode);
    os_memset(at_query_text, 0, sizeof(at_query_text) -1);
    if (mode == 2) {
        update_vision_prompt(object_vision_prompt);
    } else if (mode == 0) {
        // reset vision prompt
        update_vision_prompt("");
    }
	else if(mode == 3){
		update_vision_prompt(object_GeneraetImage_prompt);
	}

    os_printf("update prompt text:%s\r\n", at_query_text);
    os_task_create("brtc_task_update_prompt", agent_update_prompt, (void*)at_query_text, OS_TASK_PRIORITY_NORMAL, 0, NULL, 16 * 1024);
}

static void set_agent_mode(void *agent_mode) {
    char *mode_str = (char *)agent_mode;
    if (!g_engine || !brtc_running) {
        BRTC_LOG("Engine not initialized\n");
        return;
    }

    int mode = os_atoi(mode_str);
    if (mode < 0 || mode > 2) {
        BRTC_LOG("Invalid mode: %d\n", mode);
        return;
    }

    if (mode == 0) {
        // 普通语音交互模式;视频理解+图片模式
		BRTC_LOG("[MODE SET] BRTC mode: %d\r\n", mode);
        g_enable_visual = true;
        g_enable_image_generate = false;
        baidu_chat_agent_engine_send_event_to_agent(g_engine, AGENT_EVENT_DISABLE_MEDIA_GENERATE);
        baidu_chat_agent_engine_update_visual_mode(g_engine, VISION_MODE_IMAGE);
    } else if (mode == 1) {
        // 视觉理解+视频流模式
		BRTC_LOG("[MODE SET] BRTC mode: %d\r\n", mode);
        g_enable_visual = true;
        g_enable_image_generate = false;
        baidu_chat_agent_engine_send_event_to_agent(g_engine, AGENT_EVENT_DISABLE_MEDIA_GENERATE);
        baidu_chat_agent_engine_update_visual_mode(g_engine, VISION_MODE_IMAGE);
    } else if (mode == 2) {
        // 图片生成模式
		BRTC_LOG("[MODE SET] BRTC mode: %d\r\n", mode);
        g_enable_visual = false;
        g_enable_image_generate = true;
		BRTC_LOG("[MODE SET] g_enable_visual = %d,g_enable_image_generate = %d\r\n",g_enable_visual, g_enable_image_generate);
        baidu_chat_agent_engine_update_visual_mode(g_engine, VISION_MODE_IMAGE);
        baidu_chat_agent_engine_send_event_to_agent(g_engine, AGENT_EVENT_ENABLE_MEDIA_GENERATE);
		#ifdef BW_LINE_DRAWING
			update_prompt("3");
		#endif
	}
}

// AT+BRTC_CMD="cmd",param1,param2,...  AT+BRTC_CMD="set_mode",2
// cmd: send_text  param1:文本内容
// cmd: update_prompt  param1: 2:更新视觉更解prompt 0:恢复默认prompt
// cmd: send_video  param1:发送视频帧数
// cmd: set_mode  param1: 0:普通语音交互模式 1:视觉理解模式 2:图片生成模式
// cmd: restart param1: appid

void brtc_cmd(const char *cmd, char *argv[], uint32 argc) {
    if (argc < 2) {
        os_printf("cmd format error\r\n");
        return;
    }
    char *cmd_type = argv[0];
    char* param = argv[1];
    if (param == NULL || strlen(param) == 0)
    {
        os_printf("cmd param error\r\n");
        return;
    }

    os_task_func_t func = NULL;
    os_printf("cmd_type:%s, param:%s\r\n", cmd_type, param);
    if (strcmp(cmd_type, "send_text") == 0) {
        send_text(param);
    } else if (strcmp(cmd_type, "update_prompt") == 0) {
        update_prompt(param);
    } else if (strcmp(cmd_type, "send_video") == 0) {
        send_video(param);
    } else if (strcmp(cmd_type, "set_mode") == 0) {
        func = set_agent_mode;
    } else if (strcmp(cmd_type, "restart") == 0) {
        snprintf(g_appid, sizeof(g_appid), "%s", param);
        app_main();
    } else {
        os_printf("unknown cmd:%s\r\n", cmd_type); 
        return;
    } 

    if (func == NULL) {
        return;
    }
    os_task_create("brtc_task_update_prompt", func, (void*)param, OS_TASK_PRIORITY_NORMAL, 0, NULL, 16 * 1024);
}


