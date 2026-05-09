#include "typesdef.h"
#include "errno.h"
#include "list.h"
#include "dev.h"
#include "devid.h"
#include "list.h"
#include "../../sdk/include/osal/string.h"
#include "osal/mutex.h"
#include "osal/semaphore.h"
#include "osal/mutex.h"
#include "osal/task.h"
#include "osal/timer.h"
#include "osal/work.h"
#include "osal/sleep.h"
#include "osal/string.h"
#include "lib/net/utils.h"
//#include "lib/net/skmonitor/skmonitor.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/sys.h"
#include "lwip/ip_addr.h"
#include "lwip/tcpip.h"
#include "lwip/dns.h"
#include "netif/ethernetif.h"
#include "event.h"
#include "sntp.h"

#define NTP_LI              0
#define NTP_VN              3
#define NTP_MODE            3
#define NTP_STRATUM         0
#define NTP_POLL            0
#define NTP_PRECISION       0
#define NTP_TIMESTAMP_DELTA 0x83aa7e80
#define NTP_CONV_FRAC32(x)  (uint64) ((x) * ((uint64)1<<32))
#define NTP_REVE_FRAC32(x)  ((double) ((double) (x) / ((uint64)1<<32)))
#define NTP_CONV_FRAC16(x)  (uint32) ((x) * ((uint32)1<<16))
#define NTP_REVE_FRAC16(x)  ((double)((double) (x) / ((uint32)1<<16)))
#define USEC2FRAC(x)        ((uint32) NTP_CONV_FRAC32( (x) / 1000000.0 ))
#define FRAC2USEC(x)        ((uint32) NTP_REVE_FRAC32( (x) * 1000000.0 ))
#define NTP_LFIXED2DOUBLE(x)  ((double) (ntohl(((struct l_fixedpt *) (x))->intpart) - NTP_TIMESTAMP_DELTA + FRAC2USEC(ntohl(((struct l_fixedpt *) (x))->fracpart)) / 1000000.0 ))

struct s_fixedpt {
    uint16    intpart;
    uint16    fracpart;
};

struct l_fixedpt {
    uint32    intpart;
    uint32    fracpart;
};

struct ntp_packet {
    uint8               ntp_mode: 3, ntp_vn: 3, ntp_li: 2;
    uint8               ntp_stratum;
    uint8               ntp_poll;
    int8                ntp_precision;
    struct s_fixedpt    ntp_rtdelay;
    struct s_fixedpt    ntp_rtdispersion;
    uint32              ntp_refid;
    struct l_fixedpt    ntp_refts;
    struct l_fixedpt    ntp_orits;
    struct l_fixedpt    ntp_recvts;
    struct l_fixedpt    ntp_transts;
};

struct ntp_client {
    int32  sock;
    char  *hostname;
    uint32 svr_ip;
    uint32 update_interval;
    uint32 ntp_update;
    void *update_event;
} sntp;

static void sntp_get_local_time(struct timeval *tv)
{
    gettimeofday(tv, NULL);
}

static void sntp_set_local_time(struct timeval *tv)
{
    settimeofday((const struct timeval *)tv, NULL);
}

static void sntp_check_svr_ip(void)
{
    struct hostent *host;
    const ip_addr_t *dns = dns_getserver(0);

    if (sntp.svr_ip == 0 || sntp.svr_ip == 0xffffffff) {
        if (dns && dns->addr && sntp.hostname) {
            host = gethostbyname_async(sntp.hostname);
            if (host && host->h_addr_list[0]) {
                sntp.svr_ip = ((struct in_addr *)host->h_addr_list[0])->s_addr;
                os_printf("NTP: dns %s -> ip:"IPSTR" ip->%x\r\n", sntp.hostname, IP2STR_H(ntohl(sntp.svr_ip)),sntp.svr_ip);
            } else {
                sntp.svr_ip = 0x58066bcb;
                os_printf("NTP: dns %s -> ip:%xfail\r\n", sntp.hostname,sntp.svr_ip);
            }
        }
    }
}

static void sntp_send_request(void)
{
    socklen_t tolen = sizeof(struct sockaddr_in);
    struct sockaddr_in serv_addr;
    struct timeval tv;
    struct ntp_packet packet;

    os_memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(123);
    serv_addr.sin_addr.s_addr = sntp.svr_ip;

    os_memset(&packet, 0, sizeof(struct ntp_packet));
    packet.ntp_li = NTP_LI;
    packet.ntp_vn = NTP_VN;
    packet.ntp_mode = NTP_MODE;
    packet.ntp_stratum = NTP_STRATUM;
    packet.ntp_poll = NTP_POLL;
    packet.ntp_precision = NTP_PRECISION;

    sntp_get_local_time(&tv);
    packet.ntp_transts.intpart  = htonl(tv.tv_sec + NTP_TIMESTAMP_DELTA);
    packet.ntp_transts.fracpart = htonl(USEC2FRAC(tv.tv_usec));
    os_printf("=====> sntp send reuqest 1\n");
    for(int i = 0; i < sizeof(packet);i++ )  {
        os_printf("%02x ",((uint8_t*)&packet)[i]);
    }
    os_printf("\n=====> sntp send reuqest 2\n");
    sendto(sntp.sock, &packet, sizeof(struct ntp_packet), 0, (const struct sockaddr *)&serv_addr, tolen);
}

static double sntp_calc_offset(const struct ntp_packet *ntp, const struct timeval *recvtv)
{
    double t1, t2, t3, t4;
    t1 = NTP_LFIXED2DOUBLE(&ntp->ntp_orits);
    t2 = NTP_LFIXED2DOUBLE(&ntp->ntp_recvts);
    t3 = NTP_LFIXED2DOUBLE(&ntp->ntp_transts);
    t4 = recvtv->tv_sec + recvtv->tv_usec / 1000000.0;
    return ((t2 - t1) + (t3 - t4)) / 2;
}

static void sntp_client_timer_event(void *ei, void *d)
{
    os_printf("@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
    struct event* e = (struct event*)ei;
    struct ntp_client *sntp = (struct ntp_client*)d;
    sntp_check_svr_ip();

    if (sntp->svr_ip && sntp->svr_ip != 0xffffffff) {
        sntp_send_request();
        if(!sntp->ntp_update)
        {
            eloop_set_event_interval(e,5*1000);
            os_printf("first set***********\n");
        }
        //如果已经获取过时间,则1小时再重新更新
        else
        {
            eloop_set_event_interval(e,60*60*1000);
        }
    }else{
        eloop_set_event_interval(e,1000);
    }
    return 0;
}

static void sntp_task_eloop(void *ei, void *d)
{
    int32 ret;
    double offset;
    socklen_t from_len;
    struct timeval tv;
    struct ntp_packet packet;
    struct sockaddr_in serv_addr;
    struct ntp_client *sntp = (struct ntp_client*)d;
    int sock = sntp->sock;

    os_printf("+++SNTP:%s ,%d \n",__FUNCTION__,__LINE__);
    ret = recvfrom(sock, &packet, sizeof(struct ntp_packet), 0, (struct sockaddr *)&serv_addr, &from_len);
    if (ret > 0) {
        sntp_get_local_time(&tv);
        offset = sntp_calc_offset(&packet, &tv);
        sntp_get_local_time(&tv);
        tv.tv_sec  += (int) offset;
        tv.tv_usec += offset - (int) offset;
        sntp_set_local_time(&tv);
        sntp->ntp_update = 1;
        os_printf("############### reset sec:%d\n",tv.tv_sec);
    } else {
        os_printf("sntp recv err ret :%d\n",ret);
    }

}

void sntp_set_server_ex(char *ntp_server)
{
    if(sntp.hostname) os_free(sntp.hostname);
    sntp.hostname = os_strdup(ntp_server);
    sntp.svr_ip = inet_addr(ntp_server);
}

/*****************************************
 * 设置多少ms后更新一下sntp
*/
void sntp_client_fresh_event(uint32 after_time_ms)
{
    sntp.ntp_update = 0;
    eloop_set_event_interval(sntp.update_event,after_time_ms);
}


/************************************************************************************************
 * 获取当前日期时间
*************************************************************************************************/

static const uint32 MON1[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};	//平年
static const uint32 MON2[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};	//闰年
static const uint32 FOURYEARS = (366 + 365 +365 +365);	//每个四年的总天数
static const uint32 DAYMS = 24*3600;	//每天的毫秒数

static void GetMonthAndDay(int nDays, int *nMonth, int *nDay, bool IsLeapYear)
{
	uint32 *pMonths = IsLeapYear?MON2:MON1;
	//循环减去12个月中每个月的天数，直到剩余天数小于等于0，就找到了对应的月份
	for ( int i=0; i<12; ++i )
	{
		int nTemp = nDays - pMonths[i];
		if ( nTemp<=0 )
		{
			*nMonth = i+1;
			if ( nTemp == 0 )//表示刚好是这个月的最后一天，那么天数就是这个月的总天数了
				*nDay = pMonths[i];
			else
				*nDay = nDays;
			break;
		}
		nDays = nTemp;
	}
}


/***********************************************************************************************************
 * 获取日期,最后UTC_offset代表UTC对于标准的UNIX时间戳的偏移,单位是Hour
***********************************************************************************************************/

void sntp_get_date(uint32 *Year,uint32 *Mon,uint32 *Day,uint32 *Hour,uint32 *Minu,uint32 *Sec,uint32 UTC_offset)
{
    struct timeval tv;
    struct tm *p_tm;
    gettimeofday(&tv,NULL);
    tv.tv_sec=tv.tv_sec+(8*3600);
    printf("get system=%d\r\n",tv.tv_sec);
    p_tm=localtime(&tv);
   *Year= p_tm->tm_year + 1900;
    *Mon =p_tm->tm_mon + 1;
    *Day = p_tm->tm_mday;
    *Hour =p_tm->tm_hour;
    *Minu = p_tm->tm_min;
    *Sec=p_tm->tm_sec;
}


void get_current_time(void *ei,void *d)
{
    uint32 Year,Mon,Day,Hour,Minu,Sec,UTC_offset;
    sntp_get_date(&Year,&Mon,&Day,&Hour,&Minu,&Sec,8);//北京时间补偿8小时

    os_printf("%04d-%02d-%02d %02d:%02d:%02d\n",Year,Mon,Day,Hour,Minu,Sec);
}

char* system_current_time()
{
    uint32 Year,Mon,Day,Hour,Minu,Sec,UTC_offset;
    sntp_get_date(&Year,&Mon,&Day,&Hour,&Minu,&Sec,8);//北京时间补偿8小时

    char date[32];
    snprintf(date, sizeof(date), 
          "%04d-%02d-%02d %02d:%02d:%02d\n",Year,Mon,Day,Hour,Minu,Sec);
    return date;
}

unsigned long get_current_timestamp(void)
{
    struct timeval tv;
    gettimeofday(&tv,NULL);
    unsigned long timestamp=0;
    if (tv.tv_sec > 1704038400) {
        tv.tv_sec += 8 * 3600; //时区
        timestamp = tv.tv_sec * 1000 + tv.tv_usec / 1000;                     
    }
    return timestamp;
}


int32 sntp_client_init_ex(char *ntp_server, int32 update_interval)
{
    int32 ret;
    struct sockaddr_in local_addr;
    printf("enter sntp:%s \r\n",ntp_server);
    sntp.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sntp.sock == -1) {
        os_printf("SNTP: create socket failed\r\n");
        return RET_ERR;
    }

    fcntl(sntp.sock, F_SETFL, O_NONBLOCK);
    os_memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port   = 0;
    local_addr.sin_addr.s_addr = IPADDR_ANY;
    ret = bind(sntp.sock, (struct sockaddr *)&local_addr, sizeof(struct sockaddr));
    if (ret == -1) {
        os_printf("SNTP: bind fail\r\n");
        closesocket(sntp.sock);
        sntp.sock = -1;
        return RET_ERR;
    }
    printf("SNTP enter socket:%d \r\n",sntp.sock);
    sntp.update_interval = update_interval;
    sntp.hostname        = os_strdup(ntp_server);
    sntp.svr_ip          = inet_addr(ntp_server);
    //OS_WORK_INIT(&sntp.work, sntp_client_work, 0);
    //os_run_work(&sntp.work);
    //sock_monitor_add(sntp.sock, SOCK_MONITOR_READ, sntp_skmonitor_cb, (uint32)&sntp);
    os_printf("sntp.svr_ip:%x\n",sntp.svr_ip);
    eloop_add_fd( sntp.sock, EVENT_READ, EVENT_F_ENABLED, sntp_task_eloop, (void*)&sntp );
    sntp.update_event = eloop_add_timer(update_interval, EVENT_F_ENABLED, sntp_client_timer_event, (void*)&sntp);
    //eloop_add_timer(5*1000, EVENT_F_ENABLED, get_current_time, NULL);//获取当前时间的用例
    os_printf("SNTP init done, ntp server:%s\r\n", ntp_server);
    return RET_OK;
}

void get_back_date(struct timeval tv,struct rtc_time_type* rtc_time)
{
     struct tm *p_tm;
    tv.tv_sec+=8*3600;
     printf("get system=%d\r\n",tv.tv_sec);
    p_tm=localtime(&tv);
     rtc_time->year= p_tm->tm_year + 1900;
     rtc_time->month =p_tm->tm_mon + 1;
      rtc_time->date = p_tm->tm_mday;
     rtc_time->hour =p_tm->tm_hour;
     rtc_time->minute = p_tm->tm_min;
     rtc_time->second=p_tm->tm_sec;
}

unsigned long get_fattime(void)
 {
 struct rtc_time_type new_time;
 unsigned long retValue = 0;
 uint8_t year = 0,month = 0,day = 0,hour = 0,minuite = 0,second = 0;
 struct timeval tv;
 gettimeofday(&tv,NULL);
 get_back_date(tv,&new_time);
 if(new_time.year < 2023)
 {
    new_time.year = 2023;
 }
 retValue= ((unsigned long)(new_time.year-1980) << 25) /* Year = 2010 */
| ((unsigned long)new_time.month << 21) /* Month = 11 */
 | ( (unsigned long)new_time.date << 16) /* Day = 2 */
 | ( (unsigned long)new_time.hour << 11) /* Hour = 15 */
 | ( (unsigned long)new_time.minute << 5) /* Min = 0 */
 | ( (unsigned long)new_time.second >> 1); /* Sec = 0 */
 return retValue;
 }

 void get_now_timer(struct rtc_time_type* rtc_time)
 {
    struct timeval tv;
    struct tm *p_tm;
	gettimeofday(&tv,NULL);
    tv.tv_sec=tv.tv_sec+(8*3600);
    // printf("get system=%d\r\n",tv.tv_sec);
    p_tm=localtime(&tv);
     rtc_time->year= p_tm->tm_year + 1900;
     rtc_time->month =p_tm->tm_mon + 1;
      rtc_time->date = p_tm->tm_mday;
     rtc_time->hour =p_tm->tm_hour;
     rtc_time->minute = p_tm->tm_min;
     rtc_time->second=p_tm->tm_sec;
    return;
 }




 void sntp_init_funnion(void)
 {
    int32 sntpres = sntp_client_init_ex("ntp.aliyun.com",60);
    if (sntpres == RET_OK) {
        os_printf("sntp ok\n");
    } else {
        os_printf("====> sntp err\n");
    }
 }
 
 
uint32_t timer_n = 1718266577;
void set_local_time(uint32_t sntpTimestamp)
{
    struct timeval tv;
    tv.tv_sec = sntpTimestamp;
    sntp_set_local_time(&tv);
}


void get_local_tim()
{
    struct timeval tv;
    sntp_get_local_time(&tv);

    printf("local time is %d:%d:%d\n",tv.tv_sec/3600,tv.tv_sec%3600/60,tv.tv_sec%60);
}