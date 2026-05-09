#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "lvgl/lvgl.h"
#include "ui_manager.h"
#include "ui_wifi.h"
#include "../app_wifi_config.h"
#include "../app_power.h"
#include "../res/res_icons.h"
#include "../../syscfg.h"

extern lv_font_t lv_font_cn_16_4bpp;
extern lv_font_t lv_font_cn_18_4bpp;
#define SECTION_FONT &lv_font_cn_16_4bpp
#define LABEL_FONT  &lv_font_cn_18_4bpp

/* ========== Constants ========== */

#define SCREEN_W       240
#define SCREEN_H       320
#define STATUS_BAR_H   26
#define SECTION_H      20
#define ITEM_H         32
#define FIRST_Y        (STATUS_BAR_H + 2)
#define MAX_VISIBLE    6   /* max items visible at once (saved + online) */
#define CONNECT_TIMEOUT_MS  15000
#define REFRESH_INTERVAL_MS 30000
#define SAVED_WIFI_MAX  3

/* Colors — warm, child-friendly palette */
#define COLOR_BG_OVERLAY  lv_color_hex(0x00B4D8)  /* sky blue overlay */
#define COLOR_FONT_DARK   lv_color_hex(0x2D3436)   /* soft black */
#define COLOR_FONT_WHITE  lv_color_hex(0xFFFFFF)
#define COLOR_FONT_GRAY   lv_color_hex(0x999999)   /* gray hints */
#define COLOR_SECTION     lv_color_hex(0x636E72)    /* dark gray section headers */
#define COLOR_CARD_BG     lv_color_hex(0xF0F0F0)    /* light gray card background */
#define COLOR_DLG_BG      lv_color_hex(0xFFFFFF)
#define COLOR_RED         lv_color_hex(0xFF6B6B)    /* soft red */

/* Password */
#define PWD_MAX_LEN      63

/* Keyboard: mode-based layout with tab switching */
#define KB_KEY_W       22
#define KB_KEY_H       26
#define KB_GAP         1
#define KB_START_Y     (STATUS_BAR_H + 128)
#define KB_MAX_ITEMS   35
#define KB_ACT_W       44
#define KB_ACT_H       30
#define KB_ACT_GAP     4

typedef enum { KB_MODE_ABC, KB_MODE_NUM, KB_MODE_SYM } kb_mode_t;

#define KB_ACT_CHAR     0
#define KB_ACT_MODE_ABC 1
#define KB_ACT_MODE_NUM 2
#define KB_ACT_MODE_SYM 3
#define KB_ACT_SHIFT    4
#define KB_ACT_DEL      5
#define KB_ACT_OK       6

typedef struct {
    const char rows[3][11];
    const int  row_lens[3];
    int        row_count;
    int        total_chars;
} kb_chars_t;

static const kb_chars_t kb_abc = {
    {"qwertyuiop", "asdfghjkl", "zxcvbnm"}, {10, 9, 7}, 3, 26
};
static const kb_chars_t kb_num = {
    {"1234567890", ".,?!'-_@/"}, {10, 10}, 2, 20
};
static const kb_chars_t kb_sym = {
    {"!@#$%^&*()", "-_+=;:[]{}"}, {10, 10}, 2, 20
};

typedef struct { const char *label; int action; } kb_act_def_t;

static const kb_act_def_t kb_acts_abc[] = {
    {"123",KB_ACT_MODE_NUM}, {"#+",KB_ACT_MODE_SYM},
    {"A/a",KB_ACT_SHIFT}, {"\xe5\x88\xa0",KB_ACT_DEL}, {"OK",KB_ACT_OK},
};
static const kb_act_def_t kb_acts_num[] = {
    {"ABC",KB_ACT_MODE_ABC}, {"#+",KB_ACT_MODE_SYM},
    {"\xe5\x88\xa0",KB_ACT_DEL}, {"OK",KB_ACT_OK},
};
static const kb_act_def_t kb_acts_sym[] = {
    {"ABC",KB_ACT_MODE_ABC}, {"123",KB_ACT_MODE_NUM},
    {"\xe5\x88\xa0",KB_ACT_DEL}, {"OK",KB_ACT_OK},
};

/* ========== States ========== */

typedef enum {
    WIFI_STATE_LIST,
    WIFI_STATE_SAVED_ACTION,   /* dialog: connect / forget */
    WIFI_STATE_KEYBOARD,       /* password input overlay */
    WIFI_STATE_CONNECTING,
    WIFI_STATE_RESULT
} wifi_state_t;

/* ========== Saved WiFi Entry ========== */

typedef struct {
    char ssid[33];
    char password[PWD_MAX_LEN + 1];
    uint8 has_password;  /* 0=open, 1=WPA */
} saved_wifi_t;

static saved_wifi_t saved_list[SAVED_WIFI_MAX];
static int saved_count = 0;

/* ========== Module State ========== */

static wifi_state_t state;
static wifi_ap_info_t ap_list[WIFI_AP_MAX];
static int ap_count;
static lv_obj_t *root;
static lv_timer_t *poll_timer;
static lv_timer_t *refresh_timer;

/* Combined item list (saved + online) for unified navigation */
#define TOTAL_MAX  (SAVED_WIFI_MAX + WIFI_AP_MAX)
typedef struct {
    int type;   /* 0=saved, 1=online AP */
    int idx;    /* index into saved_list or ap_list */
} nav_item_t;

static nav_item_t nav_items[TOTAL_MAX];
static int nav_count;

/* Focus & scroll state */
static int focused;        /* absolute index in nav_items */
static int scroll_offset;  /* first visible item index */
static int visible_count;  /* how many items fit on screen */

/* Widgets */
static lv_obj_t *item_cells[TOTAL_MAX];
static lv_obj_t *lbl_section_saved;
static lv_obj_t *lbl_section_online;

/* Saved action dialog */
static lv_obj_t *dlg_saved_bg;
static lv_obj_t *dlg_btn_conn;
static lv_obj_t *dlg_btn_forget;
static int saved_action_idx;  /* which saved entry */

/* Scan status label */
static lv_obj_t *lbl_scan_status;

/* Keyboard state */
static kb_mode_t kb_mode;
static int kb_upper;        /* 0=lowercase, 1=uppercase (ABC mode only) */
static int kb_cursor;       /* current position: 0..kb_total-1 */
static int kb_char_count;   /* chars in current mode */
static int kb_action_count; /* action buttons in current mode */
static int kb_total;        /* kb_char_count + kb_action_count */
static char kb_password[PWD_MAX_LEN + 1];
static int kb_pwd_len;
static int kb_target_ap;
static lv_obj_t *kb_overlay;
static lv_obj_t *kb_lbl_ssid;
static lv_obj_t *kb_lbl_pwd;
static lv_obj_t *kb_key_objs[KB_MAX_ITEMS];
static lv_obj_t *kb_key_labels[KB_MAX_ITEMS];
static char kb_item_chars[KB_MAX_ITEMS];
static int  kb_item_action[KB_MAX_ITEMS];

/* Result */
static lv_obj_t *lbl_result;

/* Connection timeout tracking */
static uint32_t connect_start_tick;

/* Status bar */
#define BAT_LEVELS     5
static const lv_img_dsc_t *bat_icons[BAT_LEVELS] = {
    &icon_bat0, &icon_bat1, &icon_bat2, &icon_bat3, &icon_bat4
};
static lv_obj_t *wifi_status_time;
static lv_obj_t *wifi_status_bat;
static lv_obj_t *wifi_status_wifi;

static void wifi_create_status_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), STATUS_BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(bar, LV_OPA_30, 0);

    wifi_status_bat = lv_img_create(bar);
    {
        enum bat_level lvl = app_battery_get_level();
        lv_img_set_src(wifi_status_bat, (lvl >= 0 && lvl < BAT_LEVELS) ? bat_icons[lvl] : bat_icons[BAT_LEVEL_HIGH]);
    }
    lv_obj_align(wifi_status_bat, LV_ALIGN_LEFT_MID, 2, 0);

    wifi_status_time = lv_label_create(bar);
    lv_label_set_text(wifi_status_time, "00:00");
    lv_obj_align(wifi_status_time, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(wifi_status_time, lv_color_hex(0x333333), 0);

    wifi_status_wifi = lv_img_create(bar);
    lv_img_set_src(wifi_status_wifi, g_wifi_connected ? &icon_wifi : &icon_no_wifi);
    lv_obj_align(wifi_status_wifi, LV_ALIGN_RIGHT_MID, -4, 0);

    /* Register widgets for real-time status bar updates */
    ui_manager_set_bat_img(wifi_status_bat);
    ui_manager_set_wifi_img(wifi_status_wifi);
    ui_manager_set_time_label(wifi_status_time);
}

/* ========== Forward declarations ========== */

static void build_nav_items(void);
static int find_saved(const char *ssid);
static void rebuild_list_ui(void);
static void enter_list(void);
static void show_saved_dialog(int saved_idx);
static void close_saved_dialog(void);
static void show_keyboard(int ap_idx);
static void close_keyboard(void);
static void enter_connecting(const char *ssid, const char *password, int has_pwd);
static void enter_result(int success);
static void update_highlight_pos(void);
static void rebuild_visible(void);
static void poll_cb(lv_timer_t *timer);
static void refresh_cb(lv_timer_t *timer);

/* ========== Saved WiFi persistence ========== */

static void sync_profiles_to_flash(void)
{
    sys_cfgs.wifi_profile_count = (uint8)saved_count;
    for (int i = 0; i < saved_count && i < WIFI_PROFILE_MAX; i++) {
        os_strncpy(sys_cfgs.wifi_profiles[i].ssid, saved_list[i].ssid, 32);
        sys_cfgs.wifi_profiles[i].ssid[32] = 0;
        os_strncpy(sys_cfgs.wifi_profiles[i].passwd, saved_list[i].password, PASSWD_MAX_LEN);
        sys_cfgs.wifi_profiles[i].key_mgmt = saved_list[i].has_password ? WPA_KEY_MGMT_PSK : WPA_KEY_MGMT_NONE;
    }
    for (int i = saved_count; i < WIFI_PROFILE_MAX; i++) {
        os_memset(&sys_cfgs.wifi_profiles[i], 0, sizeof(sys_cfgs.wifi_profiles[i]));
    }
    syscfg_save();
}

static void load_saved_wifi(void)
{
    /* Sanitize profile count (old flash data may have garbage) */
    if (sys_cfgs.wifi_profile_count > WIFI_PROFILE_MAX)
        sys_cfgs.wifi_profile_count = 0;

    /* Load all profiles from persistent storage */
    if (saved_count == 0 && sys_cfgs.wifi_profile_count > 0) {
        int cnt = sys_cfgs.wifi_profile_count;
        if (cnt > SAVED_WIFI_MAX) cnt = SAVED_WIFI_MAX;
        for (int i = 0; i < cnt; i++) {
            os_strncpy(saved_list[i].ssid, sys_cfgs.wifi_profiles[i].ssid, 32);
            saved_list[i].ssid[32] = 0;
            os_strncpy(saved_list[i].password, sys_cfgs.wifi_profiles[i].passwd, PWD_MAX_LEN);
            saved_list[i].has_password = (sys_cfgs.wifi_profiles[i].key_mgmt != 0) ? 1 : 0;
        }
        saved_count = cnt;
    }

    /* Also check if current sys_cfgs WiFi is in the list */
    if (sys_cfgs.ssid[0] != 0 && g_wifi_connected) {
        if (find_saved((const char *)sys_cfgs.ssid) < 0) {
            if (saved_count < SAVED_WIFI_MAX) {
                os_strncpy(saved_list[saved_count].ssid, (const char *)sys_cfgs.ssid, 32);
                saved_list[saved_count].ssid[32] = 0;
                os_strncpy(saved_list[saved_count].password, sys_cfgs.passwd, PWD_MAX_LEN);
                saved_list[saved_count].has_password = (sys_cfgs.key_mgmt != 0) ? 1 : 0;
                saved_count++;
                sync_profiles_to_flash();
            }
        }
    }
}

static int find_saved(const char *ssid)
{
    for (int i = 0; i < saved_count; i++) {
        if (os_strcmp(saved_list[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

static void save_wifi(const char *ssid, const char *password, int has_pwd)
{
    /* Check if already saved */
    int idx = find_saved(ssid);
    if (idx >= 0) {
        /* Update password */
        os_strncpy(saved_list[idx].password, password, PWD_MAX_LEN);
        saved_list[idx].has_password = has_pwd;
        sync_profiles_to_flash();
        return;
    }

    /* Add new */
    if (saved_count >= SAVED_WIFI_MAX) {
        /* Remove oldest (index 0) */
        for (int i = 0; i < saved_count - 1; i++) {
            os_memcpy(&saved_list[i], &saved_list[i + 1], sizeof(saved_wifi_t));
        }
        saved_count--;
    }

    idx = saved_count++;
    os_strncpy(saved_list[idx].ssid, ssid, 32);
    saved_list[idx].ssid[32] = 0;
    os_strncpy(saved_list[idx].password, password, PWD_MAX_LEN);
    saved_list[idx].has_password = has_pwd;
    sync_profiles_to_flash();
}

static void remove_saved(int idx)
{
    if (idx < 0 || idx >= saved_count) return;
    for (int i = idx; i < saved_count - 1; i++) {
        os_memcpy(&saved_list[i], &saved_list[i + 1], sizeof(saved_wifi_t));
    }
    saved_count--;
    sync_profiles_to_flash();
}

/* ========== Timer ========== */

static void start_poll_timer(void)
{
    if (!poll_timer) {
        poll_timer = lv_timer_create(poll_cb, 500, NULL);
    }
    lv_timer_set_period(poll_timer, 500);
    lv_timer_resume(poll_timer);
}

static void stop_poll_timer(void)
{
    if (poll_timer) {
        lv_timer_del(poll_timer);
        poll_timer = NULL;
    }
}

static void start_refresh_timer(void)
{
    if (!refresh_timer) {
        refresh_timer = lv_timer_create(refresh_cb, REFRESH_INTERVAL_MS, NULL);
    }
    lv_timer_set_period(refresh_timer, REFRESH_INTERVAL_MS);
    lv_timer_resume(refresh_timer);
}

static void stop_refresh_timer(void)
{
    if (refresh_timer) {
        lv_timer_del(refresh_timer);
        refresh_timer = NULL;
    }
}

/* ========== Signal level helper ========== */

static int signal_level(int8 rssi)
{
    if (rssi >= -50) return 5;
    if (rssi >= -60) return 4;
    if (rssi >= -70) return 3;
    if (rssi >= -80) return 2;
    return 1;
}

/* ========== Rebuild list UI (shared helper) ========== */

static void rebuild_list_ui(void)
{
    lv_group_remove_all_objs(ui_manager_group());
    lv_obj_clean(root);

    /* Re-add wallpaper background after clean */
    lv_obj_t *bg = lv_img_create(root);
    lv_img_set_src(bg, &Wallpaper0);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Semi-transparent white overlay so cards are readable */
    lv_obj_t *content_bg = lv_obj_create(root);
    lv_obj_remove_style_all(content_bg);
    lv_obj_set_size(content_bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(content_bg, 0, 0);
    lv_obj_clear_flag(content_bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(content_bg, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(content_bg, LV_OPA_70, 0);

    /* Scanning status placeholder — actual label created in rebuild_visible() next to section header */
    lbl_scan_status = NULL;

    /* Bottom hint */
    lv_obj_t *hint = lv_label_create(root);
    lv_label_set_text(hint,
        "M\xe8\xbf\x94\xe5\x9b\x9e  AI\xe9\x87\x8d\xe6\x96\xb0\xe6\x89\xab\xe6\x8f\x8f");  /* M返回 AI重新扫描 */
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_text_color(hint, COLOR_FONT_GRAY, 0);
    lv_obj_set_style_text_font(hint, SECTION_FONT, 0);

    /* Reset widget pointers */
    lbl_section_saved = NULL;
    lbl_section_online = NULL;
    os_memset(item_cells, 0, sizeof(item_cells));
    scroll_offset = 0;

    build_nav_items();
    rebuild_visible();

    /* Status bar LAST so it draws on top of everything */
    wifi_create_status_bar(root);
}

/* ========== State: AP List (two-section) ========== */

static void build_nav_items(void)
{
    nav_count = 0;

    /* Only show actual saved WiFi entries (dynamic count) */
    for (int i = 0; i < saved_count; i++) {
        nav_items[nav_count].type = 0;  /* saved */
        nav_items[nav_count].idx = i;
        nav_count++;
    }

    /* Online APs (excluding already saved) */
    for (int i = 0; i < ap_count; i++) {
        if (find_saved(ap_list[i].ssid) >= 0) continue;
        nav_items[nav_count].type = 1;  /* online */
        nav_items[nav_count].idx = i;
        nav_count++;
    }
}

static int get_saved_nav_count(void)
{
    return saved_count;
}

/* Compute how many item rows fit between section headers */
static void compute_layout(int *out_saved_rows, int *out_online_rows, int *out_online_hdr_y)
{
    int saved_rows = get_saved_nav_count();
    int y = FIRST_Y;

    if (saved_rows > 0) {
        y += SECTION_H;  /* "已保存" header */
        y += saved_rows * ITEM_H;
    }

    *out_online_hdr_y = y;
    y += SECTION_H;  /* "在线WiFi" header */

    int remaining_h = SCREEN_H - y - 20;  /* 20px for bottom hint */
    int online_rows = (remaining_h > 0) ? (remaining_h / ITEM_H) : 0;
    int online_avail = nav_count - saved_rows;
    if (online_rows > online_avail) online_rows = online_avail;

    *out_saved_rows = saved_rows;
    *out_online_rows = online_rows;
}

static void rebuild_visible(void)
{
    /* Clear old item cells */
    for (int i = 0; i < TOTAL_MAX; i++) {
        if (item_cells[i]) {
            lv_obj_del(item_cells[i]);
            item_cells[i] = NULL;
        }
    }

    int saved_rows, online_rows, online_hdr_y;
    compute_layout(&saved_rows, &online_rows, &online_hdr_y);

    /* Section headers */
    if (lbl_section_saved) lv_obj_del(lbl_section_saved);
    if (lbl_section_online) lv_obj_del(lbl_section_online);
    lbl_section_saved = NULL;
    lbl_section_online = NULL;

    if (saved_rows > 0) {
        lbl_section_saved = lv_label_create(root);
        lv_label_set_text(lbl_section_saved,
            "\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98");  /* 已保存 */
        lv_obj_set_pos(lbl_section_saved, 8, FIRST_Y);
        lv_obj_set_style_text_color(lbl_section_saved, COLOR_SECTION, 0);
        lv_obj_set_style_text_font(lbl_section_saved, SECTION_FONT, 0);
    }

    lbl_section_online = lv_label_create(root);
    lv_label_set_text(lbl_section_online, "\xe5\x9c\xa8\xe7\xba\xbfWiFi");  /* 在线WiFi */
    lv_obj_set_pos(lbl_section_online, 8, online_hdr_y);
    lv_obj_set_style_text_color(lbl_section_online, COLOR_SECTION, 0);
    lv_obj_set_style_text_font(lbl_section_online, SECTION_FONT, 0);

    /* Scan status — right side of "在线WiFi" header */
    if (lbl_scan_status) { lv_obj_del(lbl_scan_status); lbl_scan_status = NULL; }
    lbl_scan_status = lv_label_create(root);
    lv_label_set_text(lbl_scan_status,
        g_wifi_scan_done ? "" : "\xe6\x89\xab\xe6\x8f\x8f\xe4\xb8\xad...");  /* 扫描中... */
    lv_obj_align(lbl_scan_status, LV_ALIGN_TOP_RIGHT, -8, online_hdr_y);
    lv_obj_set_style_text_color(lbl_scan_status, COLOR_FONT_GRAY, 0);
    lv_obj_set_style_text_font(lbl_scan_status, SECTION_FONT, 0);

    /* Calculate scroll window for online section */
    int online_start = saved_rows;
    int online_count = nav_count - online_start;
    int online_start_y = online_hdr_y + SECTION_H;

    /* Determine visible range based on scroll_offset */
    visible_count = 0;
    int y;

    /* Draw saved items — white card with green accent */
    for (int i = 0; i < saved_rows && i < nav_count; i++) {
        y = FIRST_Y + SECTION_H + i * ITEM_H;
        int ni = i;  /* nav index for saved = i */

        lv_obj_t *cell = lv_obj_create(root);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 224, ITEM_H - 4);
        lv_obj_set_pos(cell, 8, y + 1);
        lv_obj_set_style_bg_color(cell, COLOR_CARD_BG, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_80, 0);
        lv_obj_set_style_radius(cell, 8, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

        /* Green left accent bar */
        lv_obj_t *accent = lv_obj_create(cell);
        lv_obj_remove_style_all(accent);
        lv_obj_set_size(accent, 3, ITEM_H - 10);
        lv_obj_set_pos(accent, 0, 3);
        lv_obj_set_style_bg_color(accent, COLOR_BG_OVERLAY, 0);
        lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(accent, 2, 0);
        lv_obj_clear_flag(accent, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        char buf[64];
        int is_conn = (g_wifi_connected &&
                       os_strcmp(saved_list[nav_items[ni].idx].ssid, (const char *)sys_cfgs.ssid) == 0);
        os_snprintf(buf, sizeof(buf), "%s%s",
            saved_list[nav_items[ni].idx].ssid,
            is_conn ? "  *" : "");

        lv_obj_t *lbl = lv_label_create(cell);
        lv_label_set_text(lbl, buf);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_set_style_text_color(lbl, COLOR_FONT_DARK, 0);

        item_cells[ni] = cell;
        visible_count++;
    }

    /* Draw online items — white card with signal dots */
    int vis_online = online_rows;
    if (vis_online > online_count) vis_online = online_count;

    /* Clamp scroll_offset */
    if (scroll_offset > online_count - vis_online)
        scroll_offset = (online_count - vis_online > 0) ? online_count - vis_online : 0;
    if (scroll_offset < 0) scroll_offset = 0;

    for (int vi = 0; vi < vis_online; vi++) {
        int ai = vi + scroll_offset;  /* actual online index */
        int ni = online_start + ai;   /* nav index */
        if (ni >= nav_count) break;

        y = online_start_y + vi * ITEM_H;

        lv_obj_t *cell = lv_obj_create(root);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 224, ITEM_H - 4);
        lv_obj_set_pos(cell, 8, y + 1);
        lv_obj_set_style_bg_color(cell, COLOR_CARD_BG, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_80, 0);
        lv_obj_set_style_radius(cell, 8, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

        /* SSID on left */
        lv_obj_t *lbl = lv_label_create(cell);
        lv_label_set_text(lbl, ap_list[nav_items[ni].idx].ssid);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_set_style_text_color(lbl, COLOR_FONT_DARK, 0);

        /* Signal dots on right (1-5 dots) */
        char dots[8];
        int lvl = signal_level(ap_list[nav_items[ni].idx].signal);
        os_memset(dots, '.', lvl);
        dots[lvl] = 0;

        lv_obj_t *sig = lv_label_create(cell);
        lv_label_set_text(sig, dots);
        lv_obj_align(sig, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_set_style_text_color(sig, COLOR_BG_OVERLAY, 0);

        item_cells[ni] = cell;
        visible_count++;
    }

    /* Update highlight position */
    update_highlight_pos();
}

static void update_highlight_pos(void)
{
    if (focused < 0 || focused >= nav_count || !item_cells[focused]) return;

    /* Update card background: focused gets accent, others get white */
    for (int i = 0; i < nav_count; i++) {
        if (!item_cells[i]) continue;

        /* Card background color */
        if (i == focused) {
            lv_obj_set_style_bg_color(item_cells[i], COLOR_BG_OVERLAY, 0);
            lv_obj_set_style_bg_opa(item_cells[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_color(item_cells[i], COLOR_CARD_BG, 0);
            lv_obj_set_style_bg_opa(item_cells[i], LV_OPA_COVER, 0);
        }

        /* Update all child label colors: always black, signal dots green when unfocused */
        int child_cnt = lv_obj_get_child_cnt(item_cells[i]);
        for (int j = 0; j < child_cnt; j++) {
            lv_obj_t *child = lv_obj_get_child(item_cells[i], j);
            if (!child) continue;
            /* Signal dots (last child) keep green when unfocused */
            if (i != focused && j == child_cnt - 1 && nav_items[i].type == 1) {
                lv_obj_set_style_text_color(child, COLOR_BG_OVERLAY, 0);
            } else {
                lv_obj_set_style_text_color(child, COLOR_FONT_DARK, 0);
            }
        }
    }
}

static void ensure_visible(void)
{
    /* Make sure focused item is visible, adjust scroll_offset if needed */
    int saved_rows = get_saved_nav_count();
    int online_start = saved_rows;
    int _, online_rows, __;
    compute_layout(&_, &online_rows, &__);

    if (focused < saved_rows) {
        /* Saved item - always visible */
        return;
    }

    /* Online item */
    int online_idx = focused - online_start;

    if (online_idx < scroll_offset) {
        scroll_offset = online_idx;
        rebuild_visible();
    } else if (online_idx >= scroll_offset + online_rows) {
        scroll_offset = online_idx - online_rows + 1;
        rebuild_visible();
    }
}

static void enter_list(void)
{
    state = WIFI_STATE_LIST;
    focused = 0;
    rebuild_list_ui();

    os_printf("[wifi_ui] list shown, %d saved, %d aps, nav=%d\r\n",
              saved_count, ap_count, nav_count);
}

/* ========== Saved Action Dialog ========== */

static void show_saved_dialog(int saved_idx)
{
    state = WIFI_STATE_SAVED_ACTION;
    saved_action_idx = saved_idx;

    lv_group_remove_all_objs(ui_manager_group());

    dlg_saved_bg = lv_obj_create(root);
    lv_obj_remove_style_all(dlg_saved_bg);
    lv_obj_set_size(dlg_saved_bg, 200, 110);
    lv_obj_center(dlg_saved_bg);
    lv_obj_set_style_bg_color(dlg_saved_bg, COLOR_DLG_BG, 0);
    lv_obj_set_style_bg_opa(dlg_saved_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dlg_saved_bg, 8, 0);
    lv_obj_clear_flag(dlg_saved_bg, LV_OBJ_FLAG_SCROLLABLE);

    /* Title: SSID */
    lv_obj_t *lbl = lv_label_create(dlg_saved_bg);
    lv_label_set_text(lbl, saved_list[saved_idx].ssid);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_text_color(lbl, COLOR_FONT_DARK, 0);
    lv_obj_set_style_text_font(lbl, LABEL_FONT, 0);

    /* Connect button */
    dlg_btn_conn = lv_btn_create(dlg_saved_bg);
    lv_obj_set_size(dlg_btn_conn, 80, 28);
    lv_obj_align(dlg_btn_conn, LV_ALIGN_CENTER, -50, 15);
    lv_obj_set_style_bg_color(dlg_btn_conn, COLOR_BG_OVERLAY, 0);
    lv_group_add_obj(ui_manager_group(), dlg_btn_conn);

    lv_obj_t *lbl_conn = lv_label_create(dlg_btn_conn);
    lv_label_set_text(lbl_conn, "\xe8\xbf\x9e\xe6\x8e\xa5");  /* 连接 */
    lv_obj_center(lbl_conn);
    lv_obj_set_style_text_color(lbl_conn, COLOR_FONT_WHITE, 0);

    /* Forget button */
    dlg_btn_forget = lv_btn_create(dlg_saved_bg);
    lv_obj_set_size(dlg_btn_forget, 80, 28);
    lv_obj_align(dlg_btn_forget, LV_ALIGN_CENTER, 50, 15);
    lv_obj_set_style_bg_color(dlg_btn_forget, COLOR_RED, 0);
    lv_group_add_obj(ui_manager_group(), dlg_btn_forget);

    lv_obj_t *lbl_forget = lv_label_create(dlg_btn_forget);
    lv_label_set_text(lbl_forget, "\xe5\xbf\x98\xe8\xae\xb0");  /* 忘记 */
    lv_obj_center(lbl_forget);
    lv_obj_set_style_text_color(lbl_forget, COLOR_FONT_WHITE, 0);

    lv_group_focus_obj(dlg_btn_conn);

    os_printf("[wifi_ui] saved dialog for %s\r\n", saved_list[saved_idx].ssid);
}

static void close_saved_dialog(void)
{
    if (dlg_saved_bg) {
        lv_obj_del(dlg_saved_bg);
        dlg_saved_bg = NULL;
    }
    state = WIFI_STATE_LIST;
    if (focused >= nav_count) focused = nav_count - 1;
    if (focused < 0) focused = 0;
    rebuild_list_ui();
}

/* ========== Keyboard: mode-based with tab switching ========== */

static const kb_chars_t *kb_get_chars(void)
{
    if (kb_mode == KB_MODE_NUM) return &kb_num;
    if (kb_mode == KB_MODE_SYM) return &kb_sym;
    return &kb_abc;
}

static int kb_get_act_count(void)
{
    if (kb_mode == KB_MODE_NUM) return 4;
    if (kb_mode == KB_MODE_SYM) return 4;
    return 5;  /* ABC has extra shift button */
}

static const kb_act_def_t *kb_get_acts(void)
{
    if (kb_mode == KB_MODE_NUM) return kb_acts_num;
    if (kb_mode == KB_MODE_SYM) return kb_acts_sym;
    return kb_acts_abc;
}

static void kb_update_highlight(void)
{
    if (kb_lbl_pwd) {
        char dots[PWD_MAX_LEN + 4];
        os_memset(dots, '*', kb_pwd_len);
        dots[kb_pwd_len] = 0;
        lv_label_set_text(kb_lbl_pwd, dots);
    }

    for (int i = 0; i < kb_total; i++) {
        if (!kb_key_objs[i]) continue;
        if (i == kb_cursor) {
            lv_obj_set_style_bg_color(kb_key_objs[i], COLOR_BG_OVERLAY, 0);
            lv_obj_set_style_bg_opa(kb_key_objs[i], LV_OPA_COVER, 0);
            if (kb_key_labels[i])
                lv_obj_set_style_text_color(kb_key_labels[i], COLOR_FONT_WHITE, 0);
        } else {
            lv_obj_set_style_bg_color(kb_key_objs[i], COLOR_CARD_BG, 0);
            lv_obj_set_style_bg_opa(kb_key_objs[i], LV_OPA_80, 0);
            if (kb_key_labels[i])
                lv_obj_set_style_text_color(kb_key_labels[i], COLOR_FONT_DARK, 0);
        }
    }
}

static void kb_create_keys(void)
{
    const kb_chars_t *chars = kb_get_chars();
    const kb_act_def_t *acts = kb_get_acts();
    kb_char_count = chars->total_chars;
    kb_action_count = kb_get_act_count();
    kb_total = kb_char_count + kb_action_count;

    os_memset(kb_key_objs, 0, sizeof(kb_key_objs));
    os_memset(kb_key_labels, 0, sizeof(kb_key_labels));
    os_memset(kb_item_action, 0, sizeof(kb_item_action));
    os_memset(kb_item_chars, 0, sizeof(kb_item_chars));

    int idx = 0;
    int y = KB_START_Y;

    /* Character rows */
    for (int r = 0; r < chars->row_count; r++) {
        int row_w = chars->row_lens[r] * (KB_KEY_W + KB_GAP) - KB_GAP;
        int sx = (SCREEN_W - row_w) / 2;

        for (int c = 0; c < chars->row_lens[r]; c++) {
            char ch = chars->rows[r][c];
            if (kb_mode == KB_MODE_ABC && kb_upper)
                ch = ch - 'a' + 'A';

            lv_obj_t *key = lv_obj_create(kb_overlay);
            lv_obj_remove_style_all(key);
            lv_obj_set_size(key, KB_KEY_W, KB_KEY_H);
            lv_obj_set_pos(key, sx + c * (KB_KEY_W + KB_GAP), y);
            lv_obj_set_style_bg_color(key, COLOR_CARD_BG, 0);
            lv_obj_set_style_bg_opa(key, LV_OPA_80, 0);
            lv_obj_set_style_radius(key, 4, 0);
            lv_obj_clear_flag(key, LV_OBJ_FLAG_SCROLLABLE);

            char s[2] = { ch, 0 };
            lv_obj_t *lbl = lv_label_create(key);
            lv_label_set_text(lbl, s);
            lv_obj_center(lbl);
            lv_obj_set_style_text_color(lbl, COLOR_FONT_DARK, 0);
            lv_obj_set_style_text_font(lbl, SECTION_FONT, 0);

            kb_key_objs[idx] = key;
            kb_key_labels[idx] = lbl;
            kb_item_chars[idx] = ch;
            kb_item_action[idx] = KB_ACT_CHAR;
            idx++;
        }
        y += KB_KEY_H + KB_GAP;
    }

    /* Action row */
    y += KB_ACT_GAP;
    int total_w = kb_action_count * KB_ACT_W + (kb_action_count - 1) * KB_ACT_GAP;
    int sx = (SCREEN_W - total_w) / 2;

    for (int a = 0; a < kb_action_count; a++) {
        lv_obj_t *btn = lv_obj_create(kb_overlay);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, KB_ACT_W, KB_ACT_H);
        lv_obj_set_pos(btn, sx + a * (KB_ACT_W + KB_ACT_GAP), y);

        if (acts[a].action == KB_ACT_DEL) {
            lv_obj_set_style_bg_color(btn, COLOR_FONT_GRAY, 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
        } else if (acts[a].action == KB_ACT_OK) {
            lv_obj_set_style_bg_color(btn, COLOR_BG_OVERLAY, 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
        } else {
            lv_obj_set_style_bg_color(btn, COLOR_CARD_BG, 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
        }
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl;
        if (acts[a].action == KB_ACT_DEL) {
            lbl = lv_img_create(btn);
            lv_img_set_src(lbl, &delete_18x18);
            lv_obj_center(lbl);
        } else {
            lbl = lv_label_create(btn);
            lv_label_set_text(lbl, acts[a].label);
            lv_obj_center(lbl);
            lv_obj_set_style_text_color(lbl, COLOR_FONT_WHITE, 0);
            lv_obj_set_style_text_font(lbl, SECTION_FONT, 0);
        }

        kb_key_objs[idx] = btn;
        kb_key_labels[idx] = lbl;
        kb_item_action[idx] = acts[a].action;
        kb_item_chars[idx] = 0;
        idx++;
    }
}

static void kb_destroy_keys(void)
{
    for (int i = 0; i < kb_total && i < KB_MAX_ITEMS; i++) {
        if (kb_key_objs[i]) {
            lv_obj_del(kb_key_objs[i]);
            kb_key_objs[i] = NULL;
            kb_key_labels[i] = NULL;
        }
    }
}

static void kb_switch_mode(kb_mode_t new_mode)
{
    kb_destroy_keys();
    kb_mode = new_mode;
    if (new_mode != KB_MODE_ABC) kb_upper = 0;
    kb_create_keys();
    kb_cursor = 0;
    kb_update_highlight();
}

static void show_keyboard(int ap_idx)
{
    state = WIFI_STATE_KEYBOARD;
    kb_target_ap = ap_idx;
    kb_pwd_len = 0;
    os_memset(kb_password, 0, sizeof(kb_password));
    kb_mode = KB_MODE_ABC;
    kb_upper = 0;
    kb_cursor = 0;

    lv_group_remove_all_objs(ui_manager_group());

    /* Semi-transparent dark overlay */
    kb_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(kb_overlay);
    lv_obj_set_size(kb_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(kb_overlay, 0, 0);
    lv_obj_set_style_bg_color(kb_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(kb_overlay, LV_OPA_80, 0);
    lv_obj_clear_flag(kb_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* SSID label */
    kb_lbl_ssid = lv_label_create(kb_overlay);
    char title_buf[48];
    os_snprintf(title_buf, sizeof(title_buf),
        "\xe8\xbe\x93\xe5\x85\xa5\xe5\xaf\x86\xe7\xa0\x81: %s", ap_list[ap_idx].ssid);  /* 输入密码: */
    lv_label_set_text(kb_lbl_ssid, title_buf);
    lv_obj_set_pos(kb_lbl_ssid, 8, STATUS_BAR_H + 36);
    lv_obj_set_style_text_color(kb_lbl_ssid, COLOR_FONT_WHITE, 0);
    lv_obj_set_style_text_font(kb_lbl_ssid, SECTION_FONT, 0);

    /* Password input with white background */
    lv_obj_t *pwd_bg = lv_obj_create(kb_overlay);
    lv_obj_remove_style_all(pwd_bg);
    lv_obj_set_size(pwd_bg, 224, 24);
    lv_obj_set_pos(pwd_bg, 8, STATUS_BAR_H + 64);
    lv_obj_set_style_bg_color(pwd_bg, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(pwd_bg, LV_OPA_100, 0);
    lv_obj_set_style_radius(pwd_bg, 4, 0);
    lv_obj_clear_flag(pwd_bg, LV_OBJ_FLAG_SCROLLABLE);

    kb_lbl_pwd = lv_label_create(pwd_bg);
    lv_label_set_text(kb_lbl_pwd, "");
    lv_obj_align(kb_lbl_pwd, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_text_color(kb_lbl_pwd, COLOR_FONT_DARK, 0);
    lv_obj_set_style_text_font(kb_lbl_pwd, SECTION_FONT, 0);

    /* Create keyboard keys for current mode */
    kb_create_keys();
    kb_update_highlight();

    /* Hint */
    lv_obj_t *hint = lv_label_create(kb_overlay);
    lv_label_set_text(hint,
        "UP/DN\xe9\x80\x89\xe5\xad\x97 OK\xe7\xa1\xae\xe8\xae\xa4 M\xe5\x8f\x96\xe6\xb6\x88");  /* UP/DN选字 OK确认 M取消 */
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_text_color(hint, COLOR_FONT_GRAY, 0);
    lv_obj_set_style_text_font(hint, SECTION_FONT, 0);

    os_printf("[wifi_ui] keyboard for %s\r\n", ap_list[ap_idx].ssid);
}

static void close_keyboard(void)
{
    if (kb_overlay) {
        lv_obj_del(kb_overlay);
        kb_overlay = NULL;
    }
    kb_lbl_ssid = NULL;
    kb_lbl_pwd = NULL;
    os_memset(kb_key_objs, 0, sizeof(kb_key_objs));
    os_memset(kb_key_labels, 0, sizeof(kb_key_labels));

    state = WIFI_STATE_LIST;
    if (focused >= nav_count) focused = nav_count - 1;
    if (focused < 0) focused = 0;
    rebuild_list_ui();
}

/* ========== State: Connecting ========== */

static void enter_connecting(const char *ssid, const char *password, int has_pwd)
{
    state = WIFI_STATE_CONNECTING;
    lv_group_remove_all_objs(ui_manager_group());
    lv_obj_clean(root);

    /* Re-add wallpaper background after clean */
    lv_obj_t *bg = lv_img_create(root);
    lv_img_set_src(bg, &Wallpaper0);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Semi-transparent overlay so text is readable */
    lv_obj_t *ov = lv_obj_create(root);
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_50, 0);

    lv_obj_t *lbl = lv_label_create(root);
    lv_label_set_text(lbl, "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5...");  /* 正在连接... */
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_text_color(lbl, COLOR_FONT_WHITE, 0);
    lv_obj_set_style_text_font(lbl, LABEL_FONT, 0);

    lv_obj_t *lbl_ssid = lv_label_create(root);
    lv_label_set_text(lbl_ssid, ssid);
    lv_obj_align(lbl_ssid, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_text_color(lbl_ssid, COLOR_FONT_WHITE, 0);
    lv_obj_set_style_text_font(lbl_ssid, LABEL_FONT, 0);

    /* Status bar on top */
    wifi_create_status_bar(root);

    wifi_connect(ssid, password, has_pwd);
    connect_start_tick = lv_tick_get();
    start_poll_timer();

    os_printf("[wifi_ui] connecting to %s\r\n", ssid);
}

/* ========== State: Result ========== */

static void enter_result(int success)
{
    state = WIFI_STATE_RESULT;
    stop_poll_timer();
    stop_refresh_timer();
    lv_group_remove_all_objs(ui_manager_group());
    lv_obj_clean(root);

    /* Re-add wallpaper background after clean */
    lv_obj_t *bg = lv_img_create(root);
    lv_img_set_src(bg, &Wallpaper0);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Semi-transparent overlay */
    lv_obj_t *ov = lv_obj_create(root);
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_50, 0);

    lbl_result = lv_label_create(root);
    lv_label_set_text(lbl_result,
        success ? "\xe8\xbf\x9e\xe6\x8e\xa5\xe6\x88\x90\xe5\x8a\x9f"   /* 连接成功 */
                 : "\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5");  /* 连接失败 */
    lv_obj_align(lbl_result, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_text_color(lbl_result,
        success ? COLOR_FONT_WHITE : COLOR_RED, 0);
    lv_obj_set_style_text_font(lbl_result, LABEL_FONT, 0);

    /* Status bar on top */
    wifi_create_status_bar(root);

    /* Auto-pop after 2 seconds */
    if (poll_timer) lv_timer_del(poll_timer);
    poll_timer = lv_timer_create(poll_cb, 2000, NULL);

    os_printf("[wifi_ui] result: %s\r\n", success ? "success" : "fail");
}

/* ========== Poll Timer Callback ========== */

static void poll_cb(lv_timer_t *timer)
{
    if (state == WIFI_STATE_LIST) {
        /* Update scan status label */
        if (lbl_scan_status) {
            lv_label_set_text(lbl_scan_status,
                g_wifi_scan_done ? "" : "\xe6\x89\xab\xe6\x8f\x8f\xe4\xb8\xad...");  /* 扫描中... */
        }

        /* Only fetch results after scan completes to avoid race with wifi_on_scan_done */
        if (g_wifi_scan_done) {
            int new_count = wifi_scan_get_results(ap_list, WIFI_AP_MAX);
            if (new_count != ap_count) {
                ap_count = new_count;
                if (focused >= nav_count) focused = (nav_count > 0) ? nav_count - 1 : 0;
                rebuild_list_ui();
                os_printf("[wifi_ui] live update: %d aps\r\n", ap_count);
            }
            stop_poll_timer();
            start_refresh_timer();
        }
    }
    else if (state == WIFI_STATE_CONNECTING) {
        if (g_wifi_connected) {
            const char *ssid = (const char *)sys_cfgs.ssid;
            const char *pwd = sys_cfgs.passwd;
            save_wifi(ssid, pwd, (sys_cfgs.key_mgmt != 0) ? 1 : 0);
            enter_result(1);
        } else if (lv_tick_get() - connect_start_tick >= CONNECT_TIMEOUT_MS) {
            os_printf("[wifi_ui] connect timeout (%d ms)\r\n", CONNECT_TIMEOUT_MS);
            enter_result(0);
        }
    }
    else if (state == WIFI_STATE_RESULT) {
        stop_poll_timer();
        ui_manager_pop();
    }
}

static void refresh_cb(lv_timer_t *timer)
{
    /* Auto re-scan while on list page */
    if (state == WIFI_STATE_LIST) {
        os_printf("[wifi_ui] auto-refresh scan\r\n");
        g_wifi_scan_done = 0;
        wifi_scan_start();
        start_poll_timer();
    }
}

/* ========== Page Callbacks ========== */

static lv_obj_t *wifi_scan_create(ui_page_t *page, lv_obj_t *parent)
{
    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* Full-screen wallpaper background (same as main menu) */
    lv_obj_t *bg = lv_img_create(root);
    lv_img_set_src(bg, &Wallpaper0);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    os_memset(item_cells, 0, sizeof(item_cells));
    poll_timer = NULL;
    refresh_timer = NULL;
    dlg_saved_bg = NULL;
    dlg_btn_conn = NULL;
    dlg_btn_forget = NULL;
    kb_overlay = NULL;
    kb_lbl_ssid = NULL;
    kb_lbl_pwd = NULL;
    os_memset(kb_key_objs, 0, sizeof(kb_key_objs));
    os_memset(kb_key_labels, 0, sizeof(kb_key_labels));
    lbl_section_saved = NULL;
    lbl_section_online = NULL;
    lbl_result = NULL;
    scroll_offset = 0;
    focused = 0;
    nav_count = 0;

    load_saved_wifi();
    ap_count = 0;

    /* Start scan and enter list directly (real-time updates via poll timer) */
    os_printf("[wifi_ui] create: load_saved done, saved=%d\r\n", saved_count);
    g_wifi_scan_done = 0;
    wifi_scan_start();
    os_printf("[wifi_ui] create: scan started, entering list\r\n");
    enter_list();
    os_printf("[wifi_ui] create: list entered, starting poll\r\n");
    start_poll_timer();

    return root;
}

static void wifi_scan_destroy(ui_page_t *page)
{
    stop_poll_timer();
    stop_refresh_timer();
    lbl_scan_status = NULL;
    wifi_status_time = NULL;
    wifi_status_bat = NULL;
    wifi_status_wifi = NULL;
    dlg_saved_bg = NULL;
    dlg_btn_conn = NULL;
    dlg_btn_forget = NULL;
    kb_overlay = NULL;
    os_memset(kb_key_objs, 0, sizeof(kb_key_objs));
    os_memset(kb_key_labels, 0, sizeof(kb_key_labels));
    os_memset(item_cells, 0, sizeof(item_cells));
    root = NULL;

    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
}

/* ========== Key Handler ========== */

static void wifi_scan_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    if (evt != KEY_EVT_SHORT) return;

    /* Global: M key */
    if (id == KEY_ID_M) {
        if (state == WIFI_STATE_KEYBOARD) {
            close_keyboard();
            return;
        }
        if (state == WIFI_STATE_SAVED_ACTION) {
            close_saved_dialog();
            return;
        }
        stop_refresh_timer();
        ui_manager_pop();
        return;
    }

    /* State: LIST navigation */
    if (state == WIFI_STATE_LIST) {
        if (id == KEY_ID_AI) {
            /* Re-scan */
            stop_refresh_timer();
            g_wifi_scan_done = 0;
            wifi_scan_start();
            start_poll_timer();
            return;
        }

        if (nav_count == 0) return;

        if (id == KEY_ID_UP) {
            /* UP = move focus up (previous item) */
            if (focused > 0) focused--;
            else focused = nav_count - 1;
            ensure_visible();
            update_highlight_pos();
            os_printf("[wifi_ui] UP -> focus %d\r\n", focused);
            return;
        }
        if (id == KEY_ID_DOWN) {
            /* DOWN = move focus down (next item) */
            if (focused < nav_count - 1) focused++;
            else focused = 0;
            ensure_visible();
            update_highlight_pos();
            os_printf("[wifi_ui] DOWN -> focus %d\r\n", focused);
            return;
        }

        if (id == KEY_ID_OK) {
            if (focused < 0 || focused >= nav_count) return;
            if (nav_items[focused].type == 0) {
                /* Saved WiFi → show connect/forget dialog */
                show_saved_dialog(nav_items[focused].idx);
            } else {
                /* Online WiFi → show keyboard */
                show_keyboard(nav_items[focused].idx);
            }
            return;
        }
    }

    /* State: SAVED_ACTION dialog */
    if (state == WIFI_STATE_SAVED_ACTION) {
        lv_group_t *grp = ui_manager_group();
        lv_obj_t *foc = lv_group_get_focused(grp);

        if (id == KEY_ID_OK) {
            if (foc == dlg_btn_forget) {
                /* Forget — disconnect WiFi and remove saved entry */
                os_printf("[wifi_ui] forget saved[%d]=%s\r\n",
                          saved_action_idx, saved_list[saved_action_idx].ssid);
                wifi_disconnect();
                remove_saved(saved_action_idx);
                close_saved_dialog();
            } else {
                /* Connect */
                os_printf("[wifi_ui] reconnect saved[%d]=%s\r\n",
                          saved_action_idx, saved_list[saved_action_idx].ssid);
                enter_connecting(saved_list[saved_action_idx].ssid,
                                 saved_list[saved_action_idx].password,
                                 saved_list[saved_action_idx].has_password);
            }
            return;
        }

        if (id == KEY_ID_UP || id == KEY_ID_DOWN) {
            /* Toggle between two buttons */
            if (foc == dlg_btn_conn && dlg_btn_forget)
                lv_group_focus_obj(dlg_btn_forget);
            else if (dlg_btn_conn)
                lv_group_focus_obj(dlg_btn_conn);
            return;
        }
        return;
    }

    /* State: KEYBOARD (mode-based, UP/DOWN linear navigation) */
    if (state == WIFI_STATE_KEYBOARD) {
        if (id == KEY_ID_UP) {
            if (kb_cursor > 0) kb_cursor--;
            else kb_cursor = kb_total - 1;
            kb_update_highlight();
            return;
        }
        if (id == KEY_ID_DOWN) {
            if (kb_cursor < kb_total - 1) kb_cursor++;
            else kb_cursor = 0;
            kb_update_highlight();
            return;
        }
        if (id == KEY_ID_OK) {
            int act = kb_item_action[kb_cursor];
            if (act == KB_ACT_CHAR) {
                if (kb_pwd_len < PWD_MAX_LEN) {
                    kb_password[kb_pwd_len++] = kb_item_chars[kb_cursor];
                    kb_update_highlight();
                    os_printf("[wifi_ui] kb char '%c', len=%d\r\n",
                              kb_item_chars[kb_cursor], kb_pwd_len);
                }
            } else if (act == KB_ACT_DEL) {
                if (kb_pwd_len > 0) {
                    kb_password[--kb_pwd_len] = 0;
                    kb_update_highlight();
                }
            } else if (act == KB_ACT_OK) {
                os_printf("[wifi_ui] kb done, pwd='%s'\r\n", kb_password);
                int has_pwd = (kb_pwd_len > 0) ? 1 : 0;
                /* Debug: print all saved WiFi entries */
                os_printf("[wifi_ui] saved_count=%d\r\n", saved_count);
                for (int si = 0; si < saved_count; si++) {
                    os_printf("[wifi_ui] saved[%d] ssid='%s' pwd='%s' has_pwd=%d\r\n",
                        si, saved_list[si].ssid,
                        saved_list[si].password,
                        saved_list[si].has_password);
                }
                enter_connecting(ap_list[kb_target_ap].ssid,
                                 kb_password, has_pwd);
            } else if (act == KB_ACT_SHIFT) {
                kb_upper = !kb_upper;
                kb_destroy_keys();
                kb_create_keys();
                kb_update_highlight();
            } else if (act == KB_ACT_MODE_ABC) {
                kb_switch_mode(KB_MODE_ABC);
            } else if (act == KB_ACT_MODE_NUM) {
                kb_switch_mode(KB_MODE_NUM);
            } else if (act == KB_ACT_MODE_SYM) {
                kb_switch_mode(KB_MODE_SYM);
            }
            return;
        }
        if (id == KEY_ID_AI) {
            if (kb_pwd_len > 0) {
                kb_password[--kb_pwd_len] = 0;
                kb_update_highlight();
            }
            return;
        }
        return;
    }
}

/* ========== Page Instance ========== */

ui_page_t wifi_scan_page = {
    .id       = PAGE_WIFI_SCAN,
    .create   = wifi_scan_create,
    .destroy  = wifi_scan_destroy,
    .on_key   = wifi_scan_on_key,
    .root_obj = NULL,
};
