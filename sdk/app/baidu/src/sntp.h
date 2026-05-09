#ifndef __SNTP_H
#define __SNTP_H
#include "typesdef.h"
#include "../../include/hal/rtc.h"

int32 sntp_client_init_ex(char *ntp_server, int32 update_interval);
void sntp_get_date(uint32 *Year,uint32 *Mon,uint32 *Day,uint32 *Hour,uint32 *Minu,uint32 *Sec,uint32 UTC_offset);
void sntp_client_fresh_event(uint32 after_time_ms);
void get_now_timer(struct rtc_time_type* rtc_time);
unsigned long get_fattime(void);
void get_back_date(struct timeval tv,struct rtc_time_type* rtc_time);
unsigned long get_current_timestamp(void);
char* system_current_time(void);
#endif