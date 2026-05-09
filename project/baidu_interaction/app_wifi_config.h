#ifndef APP_WIFI_CONFIG_H
#define APP_WIFI_CONFIG_H

#include "typesdef.h"

#define WIFI_AP_MAX     20

typedef struct {
    char    ssid[33];
    uint8   bssid[6];
    int8    signal;     /* RSSI dBm */
    uint8   encrypt;    /* 0=open, non-zero=WPA */
    uint16  freq;       /* MHz */
} wifi_ap_info_t;

/* Start asynchronous WiFi scan. Returns 0 on success. */
int wifi_scan_start(void);

/* Get scan results. Returns number of APs found (0 if scan not done yet). */
int wifi_scan_get_results(wifi_ap_info_t *aps, int max_count);

/* Connect to AP. Saves config to sys_cfgs and flushes. */
void wifi_connect(const char *ssid, const char *password, int has_password);

/* Disconnect current WiFi, clear saved config. */
void wifi_disconnect(void);

/* Status flags — set by main.c WiFi event callback */
extern volatile uint8 g_wifi_scan_done;
extern volatile uint8 g_wifi_connected;

#endif /* APP_WIFI_CONFIG_H */
