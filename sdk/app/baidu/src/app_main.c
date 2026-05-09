#include "sys_config.h"
#include "typesdef.h"
#include "list.h"
#include "osal/task.h"
#include "osal_file.h"
#include "custom_mem/custom_mem.h"
#include "curl/curl.h"
#include "cJSON.h"

#include "../include/baidu_chat_agents_engine.h"
#include "../include/baidu_rtc_client.h"
#include "playback/playback.h"
#include "txw81x_voice.h"
#include "txw81x_video.h"
#include "video_raw_data.h"
#include "printer/printer_image.h"
#include "printer/printer_voice.h"

// 中山：
/* http://106.12.120.112:8936/api/aiagent
 * 
 * 拍学机：
 * appse5qw343eugn
 * 4e794ce971a24213b569b643e3fa306b
 * */


// 客户对接环境 
#define SERVER_HOST_BD_DEV              "http://106.13.69.123:8936/api/v1/aiagent"
//#define SERVER_HOST_BD_DEV              "http://ai.agent.kaywang.cn:8988/test/v1/aiagent"
// 内部研发测试环境（不对外）
// #define SERVER_HOST_BD_DEV              "http://ai.agent.kaywang.cn:8988/test/v1/aiagent"

#define SERVER_HOST_DEV_AMERICA              "线下提供"
// 上线时须切换为用户自已的线上服务地址
#define SERVER_HOST_USE_ONLINE              ""

//文生图使用irag
#define BDCloudRTCAppID          "appscnr65vrkka1" // REGION_BD_DEV APPID
//#define BDCloudRTCAppID          "apppibr915pu75m"//"appscnr65vrkka1" // REGION_BD_DEV APPID
// 支持云音乐播放、图生图使用imageprocess
//#define BDCloudRTCAppID          "appqb1g7txx1k8q"
// #define BDCloudRTCAppID          "appmdty71uuwx8u"       // REGION_AMERICA APPID

// BRTC 客户对接环境访问地址
#define JSON_CONFIG_TEMPLATE "{\"app_id\": \"%s\",  \"config\" : \"{\\\"llm\\\" : \\\"%s\\\",\\\"dfda\\\" : \\\"true\\\", \\\"remote_music_player\\\" : \\\"true\\\", \\\"llm_token\\\" : \\\"no\\\", \\\"rtc_ac\\\": \\\"pcmu\\\", \\\"lang\\\" : \\\"%s\\\"}\", \"quick_start\": true}"
//#define JSON_CONFIG_TEMPLATE_VISUAL "{\"app_id\": \"%s\", \"config\" : \"{\\\"llm\\\" : \\\"%s\\\", \\\"llm_token\\\" : \\\"no\\\", \\\"enable_visual\\\" : \\\"%s\\\", \\\"dfda\\\" : \\\"true\\\",\\\"remote_music_player\\\" : \\\"true\\\", \\\"rtc_ac\\\": \\\"pcmu\\\", \\\"lang\\\" : \\\"%s\\\", \\\"tts_enable_fast_send\\\": \\\"%s\\\", \\\"tts_fast_send_second\\\": \\\"2\\\", \\\"tts_fast_send_ratio\\\": \\\"2\\\"}\", \"quick_start\": true}"
#define JSON_CONFIG_TEMPLATE_VISUAL "{\"app_id\": \"%s\", \"config\" : \"{\\\"llm\\\" : \\\"%s\\\", \\\"llm_token\\\" : \\\"no\\\", \\\"enable_visual\\\" : \\\"%s\\\", \\\"screen_width\\\" : \\\"480\\\", \\\"screen_height\\\" : \\\"640\\\", \\\"dfda\\\" : \\\"true\\\",\\\"remote_music_player\\\" : \\\"true\\\", \\\"rtc_ac\\\": \\\"pcmu\\\", \\\"lang\\\" : \\\"%s\\\"}\", \"quick_start\": true}"
#define MAX_APPID_LEN 64
#define MAX_ROOMNAME_LEN 128
#define MAX_PARAM_LENGTH 2048
#define HTTP_TIMEOUT_MS 10000
#define DEFAULT_BRTC_LLM "LLMRacing"  // 内置默认模型， VOLC/XUNFEI/BAIDU 通过竞速命中
#define DEFAULT_BRTC_LANG "zh"        // 默认语言
#define BRTC_LOG os_printf
// #define DUMP_VIDEO_FRAME
#define FAST_SEND_BUF_SIZE (320*125) // 125帧音频数据大小约2500ms缓存，单位Byte

#define BW_LINE_DRAWING

static bool brtc_running = false;
static volatile bool g_ptt_active = false;  /* [新增] PTT 按键说话门控标志，true=发送音频到BRTC */
static uint64_t time_last_asr = 0;
static int is_first_audio = 1;
uint8_t curl_initialized = 0;

/* 使用全局引擎变量，方便各任务访问 */
static BaiduChatAgentEngine *g_engine = NULL;
struct ausrc_st *ausrc;  
struct auplay_st *auplay;  
static bool g_brtc_license_abnormal_exit = false;    //true:表示鉴权失败退出

static bool g_enable_internal_audio = false;         // true: 音频内部采集、播放（默认） false:音频外部采集、播放 （用户需对音频二次处理时使用）
static bool g_enable_tts_fast_send = false;
static bool g_enable_local_agent = false;            // true：SDK内部请求客户对接环境创建智能体（仅测试） false: 业务侧请求应用服务（AppServer）创建智能体
static bool g_enable_visual = false;                 // true: 视觉理解模式（依赖开启视频）， 与图片生成模式互斥； false: 普通语音交互模式（默认）
static bool g_vision_mode = VISION_MODE_IMAGE;       // VISION_MODE_IMAGE :视觉理解图片模式; VISION_MODE_STREAM 视觉视频流模式 (默认)
static bool g_enable_enhance_query = false;          // 用户query增强 （谨慎使用）
static bool g_enable_image_generate = true;          // 开启图片生成模式（默认开启）,依赖开启视频。与视觉理解模式互斥;

static Region region = REGION_BD_DEV;           // 当前除北美外的其它接入点暂未开放，默认使用中国大陆

char g_platform_host[256];
char g_appid [MAX_APPID_LEN]="";
static const char* object_ocr_prompt =
"# 你是一位专业的文字识别教师，你每次会收到一张包含文字的照片。请识别照片中的**所有文字内容**，并用中文和英文两种语言进行介绍。\\\\n\\\\n"
"要求：\\\\n"
"1. 准确识别图片中的所有文字，包括汉字、英文单词、数字、标点符号等\\\\n"
"2. 对识别出的**每个汉字**，提供：笔画数量(strokenumber)、偏旁部首(radical)、不同词义(comment)、包含该文字的例句(example_sentence)\\\\n"
"3. 对识别出的**每个英文单词**，提供：美式音标(soundmark_us)、英式音标(soundmark_en)、词性及中文翻译(comment)、示例语句(example_sentence)\\\\n"
"4. 如果图片中有数字，用中文和英文分别解释其含义\\\\n"
"5. 保证输出内容**简洁明了**，适合幼儿园和小学生理解\\\\n\\\\n"
"## 参考示例，如你收到一张写着\\\\\"苹果\\\\\"的图片，按同样格式输出：\\\\n"
"苹果 Apple。\\\\n"
"((CUSTOM: \\\\n"
"{\\\\n"
"      \\\\\\\"text_ch\\\\\\\": \\\\\\\"苹果\\\\\\\",\\\\n"
"      \\\\\\\"text_en\\\\\\\": \\\\\\\"Apple\\\\\\\",\\\\n"
"      \\\\\\\"comments_ch\\\\\\\": [\\\\n"
"      {\\\\n"
"        \\\\\\\"name\\\\\\\": \\\\\\\"苹\\\\\\\",\\\\n"
"        \\\\\\\"strokenumber\\\\\\\": 8,\\\\n"
"        \\\\\\\"radical\\\\\\\": \\\\\\\"艹\\\\\\\",\\\\n"
"        \\\\\\\"comment\\\\\\\": \\\\\\\"1.苹果，一种常见水果。\\\\\\\",\\\\n"
"        \\\\\\\"example_sentence\\\\\\\": \\\\\\\"我最喜欢吃红苹果。\\\\\\\"\\\\n"
"      },\\\\n"
"      {\\\\n"
"        \\\\\\\"name\\\\\\\": \\\\\\\"果\\\\\\\",\\\\n"
"        \\\\\\\"strokenumber\\\\\\\": 8,\\\\n"
"        \\\\\\\"radical\\\\\\\": \\\\\\\"木\\\\\\\",\\\\n"
"        \\\\\\\"comment\\\\\\\": \\\\\\\"1.果实，植物结出的可食部分。2.结果，事情的结局。\\\\\\\",\\\\n"
"        \\\\\\\"example_sentence\\\\\\\": \\\\\\\"树上结满了果实。\\\\\\\"\\\\n"
"      }\\\\n"
"      ],\\\\n"
"      \\\\\\\"comments_en\\\\\\\": [\\\\n"
"      {\\\\n"
"        \\\\\\\"name\\\\\\\": \\\\\\\"Apple\\\\\\\",\\\\n"
"        \\\\\\\"soundmark_us\\\\\\\": \\\\\\\"/ˈæpl/\\\\\\\",\\\\n"
"        \\\\\\\"soundmark_en\\\\\\\": \\\\\\\"/ˈæpl/\\\\\\\",\\\\n"
"        \\\\\\\"comment\\\\\\\": \\\\\\\"n. 苹果\\\\\\\",\\\\n"
"        \\\\\\\"example_sentence\\\\\\\": \\\\\\\"I eat an apple every day.\\\\\\\"\\\\n"
"      }\\\\n"
"      ]\\\\n"
"    }\\\\n"
"))\\\\n\\\\n"
"### 按以下内容来应答：\\\\n"
"<文字>:{words}。\\\\n"
"((CUSTOM: \\\\n"
"{\\\\n"
"      \\\\\\\"text_ch\\\\\\\": \\\\\\\"<中文文字>\\\\\\\",\\\\n"
"      \\\\\\\"text_en\\\\\\\": \\\\\\\"<英文翻译>\\\\\\\",\\\\n"
"      \\\\\\\"comments_ch\\\\\\\": [\\\\n"
"      {\\\\n"
"        \\\\\\\"name\\\\\\\": \\\\\\\"<单个汉字>\\\\\\\",\\\\n"
"        \\\\\\\"strokenumber\\\\\\\": XXX,\\\\n"
"        \\\\\\\"radical\\\\\\\": \\\\\\\"XXX\\\\\\\",\\\\n"
"        \\\\\\\"comment\\\\\\\": \\\\\\\"XXX\\\\\\\",\\\\n"
"        \\\\\\\"example_sentence\\\\\\\": \\\\\\\"XXX\\\\\\\"\\\\n"
"      }\\\\n"
"      ],\\\\n"
"      \\\\\\\"comments_en\\\\\\\": [\\\\n"
"      {\\\\n"
"        \\\\\\\"name\\\\\\\": \\\\\\\"<单个单词>\\\\\\\",\\\\n"
"        \\\\\\\"soundmark_us\\\\\\\": \\\\\\\"XXX\\\\\\\",\\\\n"
"        \\\\\\\"soundmark_en\\\\\\\": \\\\\\\"XXX\\\\\\\",\\\\n"
"        \\\\\\\"comment\\\\\\\": \\\\\\\"XXX\\\\\\\",\\\\n"
"        \\\\\\\"example_sentence\\\\\\\": \\\\\\\"XXX\\\\\\\"\\\\n"
"      }\\\\n"
"      ]\\\\n"
"    }\\\\n"
"))";

static const char* object_vision_prompt =
"# 你是一位资深的早教老师，你每次会收到一张来自幼儿园或小学生一个物品的照片。请识别给定照片当中的物品/物体的**具体种类**（如橡树，茉莉花，狸花猫，拉布拉多犬），并输出它的中文汉字词语名称和英文单词名称及如字典一样详细的中英文释义。其中：\\\\n\\\\n"
"一、中文释义要求：\\\\n"
"1. 根据词语<词语>中的每个文字<文字>**分开**进行回答\\\\n"
"2. 根据提供的词语<词语>中的每个文字<文字>，回答**每个文字<文字>的笔画数量**，例如`strokenumber：{number}`\\\\n"
"3. 根据提供的词语<词语>中的每个文字<文字>，回答**每个文字<文字>的偏旁部首**，例如`radical：{word}`\\\\n"
"4. 根据提供的词语<词语>中的每个文字<文字>，按次序有条理的提供**每个文字<文字>的不同词义**，并且提供1～2个在**该词义下**包含提供文字<文字>的例句\\\\n"
"5. 保证输出内容尽量**简洁明了**，并且**易于低年龄的幼儿园及小学生同学理解**\\\\n"
"6. 每一个文字的回复作为一个元素存储在一个列表中回复，**请参考示例进行回复**。\\\\n"
"二、英文释义要求：\\\\n"
"1. 根据<单词/短语>中的每个单词<单词>**分开**进行回答\\\\n"
"2. 根据提供的每个<单词>，回答**该单词的美式音标**，例如 soundmark_us：{word}\\\\n"
"3. 根据提供的每个<单词>，回答**该单词的英式音标**，例如 soundmark_en：{word}\\\\n"
"4. 根据提供的每个<单词>，按次序有条理的提供**该单词的不同词性以及中文翻译**，例如 comment：{}\\\\n"
"5. 根据提供的每个<单词>，给出1～2句包含该单词的示例语句，例如 example_sentence：{sentence}\\\\n"
"6. 每一个单词的回复作为一个元素存储在一个列表中回复，保证输出内容尽量简洁明了，并且易于低年龄的幼儿园及小学生同学理解\\\\n\\\\n"
"## 参考以下示例，如你收到一朵花的图片，按同样格式输出，注意((CUSTOM: ** ))为标签化内容必须包含\\\\n"
"樱花。\\\\n"
"cherry blossom。\\\\n"
"((CUSTOM: \\\\n"
"{\\\\n"
"      \\\\\\\"name_ch\\\\\\\": \\\\\\\"樱花\\\\\\\",\\\\n"
"      \\\\\\\"name_en\\\\\\\": \\\\\\\"cherry blossom\\\\\\\",\\\\n"
"      \\\\\\\"comments_ch\\\\\\\": [\\\\n"
"      {\\\\n"
"        \\\\\\\"name\\\\\\\": \\\\\\\"樱\\\\\\\",\\\\n"
"        \\\\\\\"strokenumber\\\\\\\": 15,\\\\n"
"        \\\\\\\"radical\\\\\\\": \\\\\\\"木\\\\\\\",\\\\n"
"        \\\\\\\"comment\\\\\\\": \\\\\\\"1.\\\\\\\"樱\\\\\\\" 指樱桃树或樱花树。词语：樱桃。\\\\\\\",\\\\n"
"        \\\\\\\"example_sentence\\\\\\\": \\\\\\\"果园里种了很多樱桃树。\\\\\\\"\\\\n"
"      },\\\\n"
"      {\\\\n"
"        \\\\\\\"name\\\\\\\": \\\\\\\"花\\\\\\\",\\\\n"
"        \\\\\\\"strokenumber\\\\\\\": 7,\\\\n"
"        \\\\\\\"radical\\\\\\\": \\\\\\\"艹\\\\\\\",\\\\n"
"        \\\\\\\"comment\\\\\\\": \\\\\\\"1. 种子植物的有性繁殖器官，由花瓣、花萼、花托、花蕊组成。词语：一朵花。\\\\\\\\\\\\n2. 可供观赏的植物。词语：花草。\\\\\\\",\\\\n"
"        \\\\\\\"example_sentence\\\\\\\": \\\\\\\"学校里种了许许多多五颜六色的花。\\\\\\\"\\\\n"
"      }\\\\n"
"      ],\\\\n"
"      \\\\\\\"comments_en\\\\\\\": [\\\\n"
"        {\\\\n"
"          \\\\\\\"name\\\\\\\": \\\\\\\"cherry\\\\\\\",\\\\n"
"          \\\\\\\"soundmark_us\\\\\\\": \\\\\\\"/ˈtʃeri/\\\\\\\",\\\\n"
"          \\\\\\\"soundmark_en\\\\\\\": \\\\\\\"/ˈtʃeri/\\\\\\\",\\\\n"
"          \\\\\\\"comment\\\\\\\": \\\\\\\"n. 樱桃；樱桃树；樱桃色\\\\\\\",\\\\n"
"          \\\\\\\"example_sentence\\\\\\\": \\\\\\\"I like eating red cherries.\\\\\\\"\\\\n"
"        },\\\\n"
"        {\\\\n"
"          \\\\\\\"name\\\\\\\": \\\\\\\"blossom\\\\\\\",\\\\n"
"          \\\\\\\"soundmark_us\\\\\\\": \\\\\\\"/ˈblɑːsəm/\\\\\\\",\\\\n"
"          \\\\\\\"soundmark_en\\\\\\\": \\\\\\\"/ˈblɒsəm/\\\\\\\",\\\\n"
"          \\\\\\\"comment\\\\\\\": \\\\\\\"n. 花；花朵；花期 v. 开花；繁荣\\\\\\\",\\\\n"
"          \\\\\\\"example_sentence\\\\\\\": \\\\\\\"The apple tree is in blossom.\\\\\\\"\\\\n"
"        }\\\\n"
"      ]\\\\n"
"    }\\\\n"
"))\\\\n\\\\n"
"### 按以下内容来应答：\\\\n"
"<汉字>:{words}。\\\\n"
"<单词/短语>:{words}。\\\\n"
"((CUSTOM: \\\\n"
"{\\\\n"
"      \\\\\\\"name_ch\\\\\\\": \\\\\\\"<汉字>\\\\\\\",\\\\n"
"      \\\\\\\"name_en\\\\\\\": \\\\\\\"<单词/短语>\\\\\\\",\\\\n"
"      \\\\\\\"comments_ch\\\\\\\": [\\\\n"
"      {\\\\n"
"        \\\\\\\"name\\\\\\\": \\\\\\\"<单个汉字>\\\\\\\",\\\\n"
"        \\\\\\\"strokenumber\\\\\\\": XXX,\\\\n"
"        \\\\\\\"radical\\\\\\\": \\\\\\\"XXX\\\\\\\",\\\\n"
"        \\\\\\\"comment\\\\\\\": \\\\\\\"XXX\\\\\\\",\\\\n"
"        \\\\\\\"example_sentence\\\\\\\": \\\\\\\"XXX\\\\\\\"\\\\n"
"      },\\\\n"
"      {\\\\n"
"        \\\\\\\"name\\\\\\\": \\\\\\\"<单个汉字>\\\\\\\",\\\\n"
"        \\\\\\\"strokenumber\\\\\\\": XXX,\\\\n"
"        \\\\\\\"radical\\\\\\\": \\\\\\\"XXX\\\\\\\",\\\\n"
"        \\\\\\\"comment\\\\\\\": \\\\\\\"XXX\\\\\\\",\\\\n"
"        \\\\\\\"example_sentence\\\\\\\": \\\\\\\"XXX\\\\\\\"\\\\n"
"      }\\\\n"
"      ],\\\\n"
"      \\\\\\\"comments_en\\\\\\\": [\\\\n"
"      {\\\\n"
"        \\\\\\\"name\\\\\\\": \\\\\\\"<单个单词>\\\\\\\",\\\\n"
"        \\\\\\\"soundmark_us\\\\\\\": \\\\\\\"XXX\\\\\\\",\\\\n"
"        \\\\\\\"soundmark_en\\\\\\\": \\\\\\\"XXX\\\\\\\",\\\\n"
"        \\\\\\\"comment\\\\\\\": \\\\\\\"XXX\\\\\\\",\\\\n"
"        \\\\\\\"example_sentence\\\\\\\": \\\\\\\"XXX\\\\\\\"\\\\n"
"      },\\\\n"
"      {\\\\n"
"        \\\\\\\"name\\\\\\\": \\\\\\\"<单个单词>\\\\\\\",\\\\n"
"        \\\\\\\"soundmark_us\\\\\\\": \\\\\\\"XXX\\\\\\\",\\\\n"
"        \\\\\\\"soundmark_en\\\\\\\": \\\\\\\"XXX\\\\\\\",\\\\n"
"        \\\\\\\"comment\\\\\\\": \\\\\\\"XXX\\\\\\\",\\\\n"
"        \\\\\\\"example_sentence\\\\\\\": \\\\\\\"XXX\\\\\\\"\\\\n"
"      }\\\\n"
"      ]\\\\n"
"    }\\\\n"
"))";


// 儿童打印机 prompt
//static char* object_GenerateImage_prompt =  
//	"# 你是一个资深的黑白简笔画创作者，生成图片时请按下面要求生成精致的黑白简笔画。\\\\n"
//	"1. 背景要求：纯白背景（#FFFFFF）；\\\\n"
//    "2. 主体要求：简易黑色线条构成，简笔画风格，无其它颜色填充；\\\\n"
//	"3. 颜色要求：白底黑线，只用纯黑线（#000000）与纯白背景（#FFFFFF），无灰度、无填色、无阴影、无高光、无纹理、无噪点。\\\\n"
//	"4. 线条要求：主体轮廓使用统一且细的单一线宽（single consistent thin stroke），干净、连续、锐利；\\\\n"
//	"5. 细节要求：允许在主体内部加入更多**同线宽的细节线条**（例如：发丝、衣褶、须眉、纹饰等）以增加精致感，但所有细节必须由与轮廓相同的单一线宽构成。\\\\n"
//	"6. 总体要求：构图需简洁、留白充分、主体明确。";

// 2. 素描风格 (Sketch)
static char* object_GenerateImage_prompt_sketch =  
    "# 你是一个资深的素描艺术家，生成图片时请按下面要求生成具有写实感的高质量纯黑白素描画。\\\\n"
    "1. 背景要求：纯白背景（#FFFFFF），突出主体；\\\\n"
    "2. 主体要求：写实风格，结构严谨，透视准确，形体块面关系明确；\\\\n"
    "3. 颜色要求：纯粹的黑白关系，仅使用纯黑（#000000）线条在纯白背景上作画，禁止任何灰色涂抹和晕染；\\\\n"
    "4. 笔触要求：完全依靠纯黑线条的疏密交叉排线（cross-hatching）来构建阴影、体积感和材质感；\\\\n"
    "5. 细节要求：亮部充分留白，暗部通过密集的纯黑交叉线条加深，明暗交界线清晰，展现严密的逻辑；\\\\n"
    "6. 总体要求：体积感和空间感极强，类似古典铜版画或大师级黑白钢笔速写，画面充满线条的肌理美。";

// 3. 卡通动漫风格 (Cartoon) - 彩色动漫卡通
static char* object_GenerateImage_prompt_cartoon =
    "# 你是一个资深的动漫卡通设计师，生成图片时请按下面要求生成色彩鲜艳的动漫卡通插画。\\\\n"
    "1. 背景要求：与主题相衬的彩色渐变或简洁卡通场景背景，色彩明快和谐；\\\\n"
    "2. 主体要求：造型夸张可爱，由圆润饱满的几何形体构成，形象生动有趣；\\\\n"
    "3. 颜色要求：使用鲜艳明亮的彩色配色，色彩饱和度高，对比鲜明，主色调突出；\\\\n"
    "4. 线条与填充：使用流畅的深色外轮廓勾边，内部使用明亮的纯色平涂填充，色彩过渡干净利落；\\\\n"
    "5. 细节要求：省略不必要的写实细节，保留最具辨识度的核心特征，画面干净有层次感；\\\\n"
    "6. 总体要求：风格轻松幽默，色彩丰富饱满，具有日系动漫或美式卡通的鲜明视觉风格。";


// 4. 高清照片画 (Photo Realistic) - 真实照片风格彩色输出
static char* object_GenerateImage_prompt_photo =
    "# 你是一个资深的专业摄影师和AI图像生成大师，请按下面要求生成高质量的彩色真实照片级图像。\\\\n"
    "1. 背景要求：根据主体内容搭配合适的自然或室内场景背景，景深虚化突出主体；\\\\n"
    "2. 主体要求：写实风格，高度还原真实物体/场景的细节、质感与光影，透视准确，色彩丰富自然；\\\\n"
    "3. 光影要求：自然光照效果，合理的高光、阴影与反射，营造真实的三维立体感；\\\\n"
    "4. 色彩要求：丰富饱满的色彩，高饱和度，真实还原物体本色，色调和谐统一；\\\\n"
    "5. 画质要求：4K级清晰度，细节丰富锐利，无噪点、无模糊、无畸变；\\\\n"
    "6. 总体要求：如同专业单反相机拍摄的高清照片，具有真实感和高级感。";

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

// ASR 语音识别结果打印
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

    baidu_chat_agent_engine_send_text_to_TTS(g_engine, "你好，我来了"); //ddsddd

    // 设置增强Query, 如需要
    if (g_enable_enhance_query) {
        baidu_chat_agent_engine_set_enhance_query(g_engine, 3, "我现在在成都", "用2个字回答问题");
    }

    if(g_enable_visual) {
        if  (g_vision_mode == VISION_MODE_STREAM) {
            baidu_chat_agent_engine_update_visual_mode(g_engine, VISION_MODE_STREAM, 180);

            // 若开启视频视觉理解， 可在此处开始周期性（1000ms一次）采集发送JPEG图片
            // auto_send_video(NULL);
            video_txw81_jpeg_alloc(&video_src, jpeg_read_handler, NULL);
        } else {
            baidu_chat_agent_engine_update_visual_mode(g_engine, VISION_MODE_IMAGE, 0);
        }
    } else if (g_enable_image_generate) {
        // 如果不使用视觉理解图片模式，则可以开启图片生成
        baidu_chat_agent_engine_send_event_to_agent(g_engine, AGENT_EVENT_ENABLE_MEDIA_GENERATE);
    }
}

// AI输出的语音对应文本
typedef void (*ocr_result_cb_t)(const char *text, int len);
static ocr_result_cb_t g_ocr_result_cb = NULL;
static const uint8_t *g_ocr_vision_jpeg = NULL;
static size_t g_ocr_vision_jpeg_len = 0;

void onAIAgentSubtitle(const char* text, int len) {
    BRTC_LOG("onAIAgentSubtitle.%s, len:%d\n", text, len);
    time_last_asr = os_jiffies();
    if (g_ocr_result_cb && text) {
        /* BRTC 传的 len 不可靠（经常为0或1），始终用 strlen */
        int actual_len = (int)os_strlen(text);
        /* 跳过 [M]: 前缀（BRTC 消息类型标记） */
        const char *payload = text;
        if (actual_len > 4 && payload[0] == '[' && payload[2] == ']' && payload[3] == ':') {
            payload += 4;
            actual_len -= 4;
        }
        if (actual_len > 0) {
            g_ocr_result_cb(payload, actual_len);
        }
    }
}

void brtc_send_vision_image(const uint8_t *jpeg_data, size_t len)
{
    if (!g_engine || !jpeg_data || len == 0) {
        os_printf("[ocr] brtc_send_vision_image: invalid params\r\n");
        return;
    }

    /* 保存 JPEG 指针，供 onVisionImageRequest 使用 */
    g_ocr_vision_jpeg = jpeg_data;
    g_ocr_vision_jpeg_len = len;

    /* 1. 切换到视觉理解图片模式 */
    g_enable_visual = true;
    g_enable_image_generate = false;
    baidu_chat_agent_engine_send_event_to_agent(g_engine, AGENT_EVENT_DISABLE_MEDIA_GENERATE);
    baidu_chat_agent_engine_update_visual_mode(g_engine, VISION_MODE_IMAGE, 0);

    /* 2. 更新视觉理解prompt为OCR文字识别prompt */
    update_vision_prompt((char *)object_ocr_prompt, 2);
    if (os_strlen(at_query_text) > 0) {
        baidu_chat_agent_engine_send_event_to_agent(g_engine, at_query_text);
    }

    /* 3. 发送图片 */
    os_printf("[ocr] sending vision image, len=%d\r\n", len);
    baidu_chat_agent_engine_send_video(g_engine, jpeg_data, len);

    /* 4. 发送文本触发识别（会触发 onVisionImageRequest，此时会发我们的 OCR 图片） */
    baidu_chat_agent_engine_send_text(g_engine, "请识别图片中的文字");
}

/* 拍照识物：使用 object_vision_prompt 识别物品 */
void brtc_send_vision_object(const uint8_t *jpeg_data, size_t len)
{
    if (!g_engine || !jpeg_data || len == 0) {
        os_printf("[recognize] brtc_send_vision_object: invalid params\r\n");
        return;
    }

    g_ocr_vision_jpeg = jpeg_data;
    g_ocr_vision_jpeg_len = len;

    g_enable_visual = true;
    g_enable_image_generate = false;
    baidu_chat_agent_engine_send_event_to_agent(g_engine, AGENT_EVENT_DISABLE_MEDIA_GENERATE);
    baidu_chat_agent_engine_update_visual_mode(g_engine, VISION_MODE_IMAGE, 0);

    update_vision_prompt((char *)object_vision_prompt, 2);
    if (os_strlen(at_query_text) > 0) {
        baidu_chat_agent_engine_send_event_to_agent(g_engine, at_query_text);
    }

    os_printf("[recognize] sending vision object image, len=%d\r\n", len);
    baidu_chat_agent_engine_send_video(g_engine, jpeg_data, len);
    baidu_chat_agent_engine_send_text(g_engine, "\xe5\x9b\xbe\xe7\x89\x87\xe4\xb8\xad\xe6\x98\xaf\xe4\xbb\x80\xe4\xb9\x88\xef\xbc\x9f"); /* 图片中是什么？ */
}

void brtc_register_ocr_callback(ocr_result_cb_t cb)
{
    g_ocr_result_cb = cb;
    if (!cb) {
        g_ocr_vision_jpeg = NULL;
        g_ocr_vision_jpeg_len = 0;
    }
}

/* OCR 页面退出时恢复图片生成模式 */
void brtc_restore_image_mode(void)
{
    if (!g_engine) return;
    g_enable_visual = false;
    g_enable_image_generate = true;
    baidu_chat_agent_engine_update_visual_mode(g_engine, VISION_MODE_IMAGE, 0);
    baidu_chat_agent_engine_send_event_to_agent(g_engine, AGENT_EVENT_ENABLE_MEDIA_GENERATE);
    os_printf("[ocr] restored image generate mode\r\n");
}

void brtc_interrupt_tts(void)
{
    if (g_engine) {
        baidu_chat_agent_engine_interrupt(g_engine);
        os_printf("[brtc] TTS interrupted\r\n");
    }
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
//     BRTC_LOG("Received audio data of length: %d\n", len);  
    if (auplay && auplay->wh) {
        auplay->wh((void *)data, len, auplay);
    }
}

void dumpVideoFrame (uint8* filename, const uint8_t *data, size_t len) 
{
    int w_len = 0;
    int frame_size = 0;
    // char filename[64] = {0};
    void *fp  = NULL;
    BRTC_LOG("Entry %s:%d\n", __FUNCTION__, __LINE__);

    // os_sprintf(filename, "0:video_frame_%04d.jpeg", (uint32_t)os_jiffies() % 9999);
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



/* AI 生图数据传递：BRTC 线程 → LVGL 线程（零 SRAM，JPEG 数据存 PSRAM） */
static uint8_t *g_gen_jpeg = NULL;
static uint32_t g_gen_jpeg_len = 0;
static volatile uint8_t g_gen_ready = 0;

/* 供 voice_gen 页面查询 */
uint8_t *voice_gen_get_jpeg(uint32_t *len)
{
    if (g_gen_ready && g_gen_jpeg) {
        *len = g_gen_jpeg_len;
        return g_gen_jpeg;
    }
    return NULL;
}

void voice_gen_release_jpeg(void)
{
    if (g_gen_jpeg) {
        custom_free_psram(g_gen_jpeg);
        g_gen_jpeg = NULL;
    }
    g_gen_jpeg_len = 0;
    g_gen_ready = 0;
}

void onVideoData(const uint8_t *data, size_t len, RtcImageType imgtype, int width, int height)
{
    BRTC_LOG("FrameReceived video data of length: %d, type: %d, width: %d, height: %d\n", len, imgtype, width, height);
#ifdef DUMP_VIDEO_FRAME
    if (imgtype == RTC_IMAGE_TYPE_JPEG) {
        if (len > 0) {
            char filename[64] = {0};
            os_sprintf(filename, "0:vi%04d.jpeg", (uint32_t)os_jiffies() % 9999);
            dumpVideoFrame(filename, data, len);
            jpeg_photo_explain(filename, 320, 240);
        }
    }
#endif
    // AI 生成的图片 → 拷贝到 PSRAM，通知 LVGL 线程解码显示
    if (len > 0 && data) {
        if (g_gen_jpeg) {
            custom_free_psram(g_gen_jpeg);
            g_gen_jpeg = NULL;
        }
        g_gen_jpeg = (uint8_t *)custom_malloc_psram(len);
        if (g_gen_jpeg) {
            memcpy(g_gen_jpeg, data, len);
            g_gen_jpeg_len = len;
            g_gen_ready = 1;
            BRTC_LOG("[tti] JPEG ready: %d bytes\n", len);
        }
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
    /* OCR 模式：发送用户拍摄的照片，而非默认静态图片 */
    if (g_ocr_result_cb && g_ocr_vision_jpeg && g_ocr_vision_jpeg_len > 0) {
        printf("onVisionImageRequest send OCR image %d bytes\n", (int)g_ocr_vision_jpeg_len);
        baidu_chat_agent_engine_send_video(g_engine, g_ocr_vision_jpeg, g_ocr_vision_jpeg_len);
    } else if (g_vision_mode == VISION_MODE_IMAGE) {
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

void onMediaGenerateResult(const char* result) {
    if (result && strlen(result) > 0) {
        BRTC_LOG("onMediaGenerateResult content: %s\n", result);
    }
}

/* [新增] PTT 开始 — 由 UI 层长按按键时调用，开启音频上行 */
void brtc_ptt_start(void) {
    g_ptt_active = true;
    os_printf("[PTT] start\r\n");
}

/* [新增] PTT 停止 — 由 UI 层松开按键时调用，关闭音频上行 */
void brtc_ptt_stop(void) {
    g_ptt_active = false;
    os_printf("[PTT] stop\r\n");
}

void ausrc_read_handler(const void *sampv, size_t sampc, void *arg) {
    if (g_ptt_active && g_engine && sampc > 0) {
        baidu_chat_agent_engine_send_audio(g_engine, (const uint8_t*)sampv, sampc);
    }
}

void setUserParameters(AgentEngineParams *params)
{
    strncpy(params->agent_platform_url, g_platform_host, sizeof(params->agent_platform_url) - 1);
    strncpy(params->appid, g_appid, sizeof(params->appid) - 1); // //需要和服务端使用同一个appId
    snprintf(params->userId, sizeof(params->userId), "%s", "12345678");        // 终端用户唯一的id号，例如手机号
    strncpy(params->cer, "./a.cer", sizeof(params->cer) - 1);
    strncpy(params->workflow, "VoiceChat", sizeof(params->workflow) - 1);
    snprintf(params->license_key, sizeof(params->license_key), "%s", "262efe85b1eb40f1b5f024405e5fdbf6"); //"xxxx"为license_key字符串，需要购买获得

    params->instance_id = 10373;
    params->verbose = true;
    params->enable_internal_device = g_enable_internal_audio;
    if(g_enable_tts_fast_send) {
        if(params->enable_internal_device) {
            params->pcm_audio_buffer_size = FAST_SEND_BUF_SIZE; // 默认为25帧音频数据大小(320*25)，单位Byte
        } 
    }
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
    params->level_voice_interrupt = 60;
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
		/* 重要 ，否者生产环境下容易连接失败 */
        /* 嵌入式 SRAM 紧张，缩小 curl 内部缓冲区（默认上传为64KB+接收16KB） */
        curl_easy_setopt(curl, CURLOPT_UPLOAD_BUFFERSIZE, 1024L);
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 1024L);
        
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

void onAgentEventUpdated(const char* event_msg, size_t len) {
	BRTC_LOG("onAgentEventUpdated: %.*s\n", (int)len, event_msg);
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
        .onMediaGenerateResult = onMediaGenerateResult,
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
        if(g_enable_tts_fast_send) {
            prm.buffer_maxsz = FAST_SEND_BUF_SIZE; // 150帧音频长度（如20ms每帧，buffer duration 最大 3s）
        }
        voice_txw81_src_alloc(&ausrc, &prm, ausrc_read_handler, NULL);
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

        if (!g_enable_local_agent) {
            sendStopAIAgentInstance();
            http_cleanup();
        }
        brtc_running = false;
    }
}

int sendGenerateAIAgentCall() {
    char config_data[MAX_PARAM_LENGTH] = {0};
    bool multi_modal = g_enable_visual || g_enable_image_generate;
    snprintf(config_data, sizeof(config_data), JSON_CONFIG_TEMPLATE_VISUAL,
                g_appid, DEFAULT_BRTC_LLM, multi_modal ? "true" : "false", DEFAULT_BRTC_LANG, g_enable_tts_fast_send ? "true" : "false");
    char request_url[MAX_PARAM_LENGTH] = {0};
    snprintf(request_url, sizeof(request_url), "%s/generateAIAgentCall", g_platform_host);
    return http_post(request_url, config_data, &call_resp);
}

int sendStopAIAgentInstance() {
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
    }

    if (g_enable_local_agent) {
         brtc_demo_init();
    } else {
        int error_code;
        for (int retry = 0; retry < 2; retry++) {
            error_code = sendGenerateAIAgentCall();
            if (error_code == 0 && call_resp.http_code == 200) {
                parse_agent_instance_json(call_resp.content);
                brtc_demo_init();
                break;
            }
            BRTC_LOG("[brtc] HTTP failed (attempt %d), error=%d, http=%ld\n",
                     retry + 1, error_code, call_resp.http_code);
            if (retry == 0) {
                http_cleanup();
                os_sleep_ms(3000);
            }
        }
    }
	
//	#ifdef BW_LINE_DRAWING
//		os_sleep_ms(500);
////		update_prompt_by_printer_mode(printer_get_mode());
//	#endif
}


struct os_task task_brtc_demo;
struct os_task task_brtc_close;
void app_main(void)
{
    if (brtc_running){
        void *close_stack = custom_malloc_psram(1024 * 16);
        if (close_stack) {
            OS_TASK_INIT2("brtc_ai_agent_task_close", &task_brtc_close, baidu_chat_agent_demo_close, NULL, OS_TASK_PRIORITY_NORMAL, close_stack, 1024 * 16);
        }
        os_sleep_ms(1000);
    }
    
    if (brtc_running == false)
    {
        {
            void *brtc_stack = custom_malloc_psram(1024 * 16);
            OS_TASK_INIT2("brtc_demo_start", &task_brtc_demo, brtc_demo_start, NULL, OS_TASK_PRIORITY_BELOW_NORMAL, brtc_stack, 1024 * 16);
        }
//		OS_TASK_INIT("brtc_demo_start", &task_brtc_demo, brtc_demo_start, NULL, OS_TASK_PRIORITY_BELOW_NORMAL, 1024 * 16);
        os_printf("start  brtc_demo_start task ...\r\n");
    }
}

/* 供外部模块查询 BRTC 引擎是否就绪 */
int is_brtc_running(void) {
    return brtc_running && g_engine != NULL;
}

void agent_send_text(void * text) {
    os_printf("agent_send_text:%s\r\n", text);
    baidu_chat_agent_engine_send_text(g_engine, (char *)text);
}

void send_text(const char *text) {
    if (text && os_strlen(text) > 0) {
        os_printf("at query text:%s\r\n", text);
        void *stack = custom_malloc_psram(16 * 1024);
        if (stack) {
            os_task_create("brtc_task_send_text", agent_send_text, (void*)text, OS_TASK_PRIORITY_NORMAL, 0, stack, 16 * 1024);
        } else {
            os_printf("[send_text] psram stack alloc failed, fallback to sync\r\n");
            agent_send_text((void*)text);
        }
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
void update_vision_prompt(char *prompt , int mode) {
	int prompt_len = prompt ? strlen(prompt) : 0;
	int total_len = prompt_len + strlen(g_updata_vision_prompt_cmd) + 64;
	char *update_prompt_cmd = (char *)custom_malloc_psram(total_len);

	if (update_prompt_cmd && mode==2) {
		snprintf(update_prompt_cmd, total_len, g_updata_vision_prompt_cmd,
					AGENT_EVENT_UPDATE_SYSTEM_PROMPT, "2", prompt);
	} else if(update_prompt_cmd && mode >= 3){
		snprintf(update_prompt_cmd, total_len, g_updata_vision_prompt_cmd,
					AGENT_EVENT_UPDATE_SYSTEM_PROMPT, "3", prompt);
	}else{
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
        update_vision_prompt(object_vision_prompt ,mode);
    } else if (mode == 0) {
        // reset vision prompt
        update_vision_prompt("",mode);
    }
	else if(mode == 3){
		os_printf("[prompt] -> 高清照片(photo)\r\n");
			update_vision_prompt(object_GenerateImage_prompt_photo,mode);
	}
	else if(mode == 4){
		os_printf("[prompt] -> 动漫卡通(cartoon)\r\n");
			update_vision_prompt(object_GenerateImage_prompt_cartoon,mode);
	}
	else if(mode == 5){
		os_printf("[prompt] -> 素描(sketch)\r\n");
			update_vision_prompt(object_GenerateImage_prompt_sketch,mode);
	}

    os_printf("update prompt text:%s\r\n", at_query_text);
    void *stack = custom_malloc_psram(16 * 1024);
    if (stack) {
        os_task_create("brtc_task_update_prompt", agent_update_prompt, (void*)at_query_text, OS_TASK_PRIORITY_NORMAL, 0, stack, 16 * 1024);
    } else {
        os_printf("[update_prompt] psram stack alloc failed, fallback to sync\r\n");
        agent_update_prompt((void*)at_query_text);
    }
}

/* 同步版本：直接在调用者线程中发送，不创建新任务（节省 16KB 栈） */
void update_prompt_sync(const char *prompt_mode)
{
    int mode = os_atoi(prompt_mode);
    os_memset(at_query_text, 0, sizeof(at_query_text) -1);
    if (mode == 2) {
        update_vision_prompt(object_vision_prompt, mode);
    } else if (mode == 0) {
        update_vision_prompt("", mode);
    }
	else if(mode == 3){
		os_printf("[prompt] -> 高清照片(photo)\r\n");
			update_vision_prompt(object_GenerateImage_prompt_photo, mode);
	}
	else if(mode == 5){
		os_printf("[prompt] -> 素描(sketch)\r\n");
			update_vision_prompt(object_GenerateImage_prompt_sketch, mode);
	}
	else if(mode == 4){
		os_printf("[prompt] -> 卡通(cartoon)\r\n");
			update_vision_prompt(object_GenerateImage_prompt_cartoon, mode);
	}

    os_printf("update prompt sync text:%s\r\n", at_query_text);
    if (g_engine && os_strlen(at_query_text) > 0) {
        baidu_chat_agent_engine_send_event_to_agent(g_engine, at_query_text);
    }
}

/* printer_mode_t (0-5) → update_prompt mode string (3-8) */
static const char * const printer_mode_to_prompt[] = {

};

/**
 * @brief 切换 BRTC AI 大模型互动实例的角色
 * @param scene_role  角色名称，如 "robot"
 *
 * 示例：
 *   brtc_switch_scene_role("robot");
 */
void brtc_switch_scene_role(const char *scene_role)
{
    if (!g_engine || !brtc_running) {
        BRTC_LOG("[switch_role] engine not ready\n");
        return;
    }
    if (!scene_role) {
        BRTC_LOG("[switch_role] invalid params\n");
        return;
    }

    char event[256];
    snprintf(event, sizeof(event),
        "[OP]:[switchSceneRole]:{\\\"app_id\\\":\\\"%s\\\",\\\"ai_agent_instance_id\\\":%s,\\\"scene_role\\\":\\\"%s\\\",\\\"tts_sayhi\\\":\\\"%s\\\"}",
        g_appid, call_resp.ai_agent_instance_id, scene_role, scene_role);

    BRTC_LOG("[switch_role] %s\n", event);
    baidu_chat_agent_engine_send_event_to_agent(g_engine, event);
}

void update_prompt_by_printer_mode(printer_mode_t mode) {
    static const char * const style_names[MODE_COUNT] = {
        "简笔画(stick)", "线稿(line)", "素描(sketch)", "动漫(anime)", "卡通(cartoon)", "水墨(ink)"
    };
    if (mode >= 0 && mode < MODE_COUNT) {
        os_printf("[prompt] update_prompt_by_printer_mode: mode=%d(%s) -> prompt_mode=%s\r\n",
                  mode, style_names[mode], printer_mode_to_prompt[mode]);
        update_prompt(printer_mode_to_prompt[mode]);
    } else {
        os_printf("[prompt] ERROR: invalid printer_mode=%d\r\n", mode);
    }
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
        g_enable_visual = true;
        g_enable_image_generate = false;
        baidu_chat_agent_engine_send_event_to_agent(g_engine, AGENT_EVENT_DISABLE_MEDIA_GENERATE);
        baidu_chat_agent_engine_update_visual_mode(g_engine, VISION_MODE_IMAGE, 0);
    } else if (mode == 1) {
        // 视觉理解+视频流模式
        g_enable_visual = true;
        g_enable_image_generate = false;
        baidu_chat_agent_engine_send_event_to_agent(g_engine, AGENT_EVENT_DISABLE_MEDIA_GENERATE);
        baidu_chat_agent_engine_update_visual_mode(g_engine, VISION_MODE_STREAM, 180);
    } else if (mode == 2) {
        // 图片生成模式
        g_enable_visual = false;
        g_enable_image_generate = true;
        baidu_chat_agent_engine_update_visual_mode(g_engine, VISION_MODE_IMAGE, 180);
        baidu_chat_agent_engine_send_event_to_agent(g_engine, AGENT_EVENT_ENABLE_MEDIA_GENERATE);
    
    }
}

// AT+BRTC_CMD="cmd",param1,param2,...
// cmd: send_text  param1:文本内容
// cmd: update_prompt  param1: 2:更新视觉更解prompt 0:恢复默认prompt
// cmd: send_video  param1:发送视频帧数
// cmd: set_mode  param1: 0:普通语音交互模式 1:视觉理解模式 2:图片生成模式
// cmd: restart param1: appid

//void brtc_cmd(const char *cmd, char *argv[], uint32 argc) {
//    if (argc < 2) {
//        os_printf("cmd format error\r\n");
//        return;
//    }
//    char *cmd_type = argv[0];
//    char* param = argv[1];
//    if (param == NULL || strlen(param) == 0)
//    {
//        os_printf("cmd param error\r\n");
//        return;
//    }
//
//    os_task_func_t func = NULL;
//    os_printf("cmd_type:%s, param:%s\r\n", cmd_type, param);
//    if (strcmp(cmd_type, "send_text") == 0) {
//        send_text(param);
//    } else if (strcmp(cmd_type, "update_prompt") == 0) {
//        update_prompt(param);
//    } else if (strcmp(cmd_type, "send_video") == 0) {
//        send_video(param);
//    } else if (strcmp(cmd_type, "set_mode") == 0) {
//        func = set_agent_mode;
//    } else if (strcmp(cmd_type, "restart") == 0) {
//        snprintf(g_appid, sizeof(g_appid), "%s", param);
//        app_main();
//    } else {
//        os_printf("unknown cmd:%s\r\n", cmd_type); 
//        return;
//    } 
//
//    if (func == NULL) {
//        return;
//    }
//    os_task_create("brtc_task_update_prompt", func, (void*)param, OS_TASK_PRIORITY_NORMAL, 0, NULL, 16 * 1024);
//}


