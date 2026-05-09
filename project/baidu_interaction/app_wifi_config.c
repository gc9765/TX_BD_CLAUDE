#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "osal/sleep.h"
#include "dev.h"
#include "devid.h"
#include "lib/umac/umac.h"
#include "syscfg.h"
#include "app_wifi_config.h"

#define SCAN_TIME    200
#define SCAN_CNT     10
#define SCAN_CHAN    0xFFF   /* all 2.4G channels */

volatile uint8 g_wifi_scan_done  = 0;
volatile uint8 g_wifi_connected  = 0;

static wifi_ap_info_t scan_results[WIFI_AP_MAX];
static int scan_count = 0;

/* ---------- Scan ---------- */

int wifi_scan_start(void)
{
    struct ieee80211_scandata param;

    g_wifi_scan_done = 0;
    scan_count = 0;  /* Clear old results to avoid race with wifi_on_scan_done */

    os_memset(&param, 0, sizeof(param));
    param.chan_bitmap = SCAN_CHAN;
    param.scan_time   = SCAN_TIME;
    param.scan_cnt    = SCAN_CNT;

    int ret = ieee80211_scan(WIFI_MODE_STA, 1, &param);
    os_printf("[wifi] scan start ret=%d\r\n", ret);
    return ret;
}

int wifi_scan_get_results(wifi_ap_info_t *aps, int max_count)
{
    /* Return cached results (old or new) even during active scan */
    int n = scan_count > max_count ? max_count : scan_count;
    if (n > 0 && aps) {
        os_memcpy(aps, scan_results, n * sizeof(wifi_ap_info_t));
    }
    return n;
}

/* Called from main.c IEEE80211_EVENT_SCAN_DONE handler */
void wifi_on_scan_done(void)
{
    struct hgic_bss_info *bss_map;
    uint8 *buf;
    int ret, i;
    int count = 0;

    buf = (uint8_t *)malloc(WIFI_AP_MAX * sizeof(struct hgic_bss_info));
    if (!buf) {
        os_printf("[wifi] scan malloc fail\r\n");
        g_wifi_scan_done = 1;
        scan_count = 0;
        return;
    }

    ret = ieee80211_get_bsslist((struct hgic_bss_info *)buf, WIFI_AP_MAX, 1);
    os_printf("[wifi] get_bsslist ret=%d, sizeof(hgic_bss_info)=%d\r\n", ret, (int)sizeof(struct hgic_bss_info));

    if (ret <= 0) {
        free(buf);
        scan_count = 0;
        g_wifi_scan_done = 1;
        return;
    }

    int bss_count = ret > WIFI_AP_MAX ? WIFI_AP_MAX : ret;
    for (i = 0; i < bss_count; i++) {
        bss_map = (struct hgic_bss_info *)(buf + i * sizeof(struct hgic_bss_info));

        /* Skip duplicate SSIDs */
        int dup = 0;
        for (int j = 0; j < count; j++) {
            if (os_strcmp(scan_results[j].ssid, (const char *)bss_map->ssid) == 0) {
                /* Keep stronger signal */
                if (bss_map->signal > scan_results[j].signal) {
                    scan_results[j].signal = bss_map->signal;
                }
                dup = 1;
                break;
            }
        }
        if (dup) continue;

        /* Validate SSID: skip entries with non-printable chars */
        int ssid_valid = 0;
        for (int k = 0; k < 32 && bss_map->ssid[k]; k++) {
            if (bss_map->ssid[k] >= 0x20 && bss_map->ssid[k] < 0x7F) {
                ssid_valid = 1;
                break;
            }
        }
        if (!ssid_valid) {
            os_printf("[wifi] skip invalid SSID entry %d\r\n", i);
            continue;
        }

        os_strncpy(scan_results[count].ssid, (const char *)bss_map->ssid, 32);
        scan_results[count].ssid[32] = 0;
        os_memcpy(scan_results[count].bssid, bss_map->bssid, 6);
        scan_results[count].signal  = bss_map->signal;
        scan_results[count].encrypt = bss_map->encrypt;
        scan_results[count].freq    = bss_map->freq;
        count++;
    }

    /* Simple bubble sort by signal strength (descending) */
    for (int a = 0; a < count - 1; a++) {
        for (int b = a + 1; b < count; b++) {
            if (scan_results[b].signal > scan_results[a].signal) {
                wifi_ap_info_t tmp;
                os_memcpy(&tmp, &scan_results[a], sizeof(tmp));
                os_memcpy(&scan_results[a], &scan_results[b], sizeof(tmp));
                os_memcpy(&scan_results[b], &tmp, sizeof(tmp));
            }
        }
    }

    free(buf);
    scan_count = count;
    g_wifi_scan_done = 1;

    os_printf("[wifi] scan done, %d APs found\r\n", count);
    for (i = 0; i < count; i++) {
        os_printf("  [%d] %s  rssi=%d  enc=%d\r\n",
            i, scan_results[i].ssid, scan_results[i].signal, scan_results[i].encrypt);
    }
}

/* ---------- Connect ---------- */

void wifi_connect(const char *ssid, const char *password, int has_password)
{
    os_printf("[wifi] connect to '%s' key=%d\r\n", ssid, has_password);
    if (has_password && password) {
        os_printf("[wifi] password='%s' len=%d\r\n", password, os_strlen(password));
    }

    /* Save to sys_cfgs */
    os_strcpy((char *)sys_cfgs.ssid, ssid);
    if (has_password && password) {
        os_strcpy(sys_cfgs.passwd, password);
        sys_cfgs.key_mgmt = WPA_KEY_MGMT_PSK;
        /* Compute PSK so wificfg_flush() uses the correct key */
        wpa_passphrase((uint8 *)sys_cfgs.ssid, sys_cfgs.passwd, sys_cfgs.psk);
    } else {
        sys_cfgs.passwd[0] = 0;
        sys_cfgs.key_mgmt = WPA_KEY_MGMT_NONE;
        os_memset(sys_cfgs.psk, 0, sizeof(sys_cfgs.psk));
    }
    sys_cfgs.wifi_mode = WIFI_MODE_STA;
    sys_cfgs.dhcpc_en  = 1;
    sys_cfgs.station_channel = 0;   /* full channel scan */

    wifi_create_station((char *)sys_cfgs.ssid,
                        sys_cfgs.passwd,
                        sys_cfgs.key_mgmt);

    syscfg_flush(1);

    g_wifi_connected = 0;
}

/* ---------- Disconnect ---------- */

void wifi_disconnect(void)
{
    os_printf("[wifi] disconnect\r\n");

    ieee80211_iface_stop(WIFI_MODE_STA);

    os_memset(sys_cfgs.ssid, 0, sizeof(sys_cfgs.ssid));
    os_memset(sys_cfgs.passwd, 0, sizeof(sys_cfgs.passwd));
    sys_cfgs.key_mgmt = 0;
    sys_cfgs.wifi_mode = WIFI_MODE_STA;
    sys_cfgs.dhcpc_en  = 0;
    sys_cfgs.station_channel = 0;

    g_wifi_connected = 0;
    syscfg_flush(1);
}
