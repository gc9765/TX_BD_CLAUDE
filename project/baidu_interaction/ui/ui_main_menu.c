#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "lvgl/lvgl.h"
#include "ui_manager.h"
#include "ui_main_menu.h"
#include "../res/res_icons.h"
#include "../app_power.h"
#include "../app_wifi_config.h"

extern int is_brtc_running(void);
extern lv_font_t lv_font_cn_18_4bpp;
#define POPUP_FONT &lv_font_cn_18_4bpp

/* ========== Layout ==========
 * LVGL 240x320 portrait, no rotation.
 * 2 cols x 4 rows, icon only, no highlight.
 * Focus = 50px/60px icon blink via lv_timer.
 * Cell 120w x 70h, Status bar 26px.
 */

#define MENU_COLS      2
#define MENU_COUNT     8

#define STATUS_BAR_H   26
#define CELL_W         120
#define CELL_H         70
#define ROW_GAP        2
#define COL_GAP        0
#define FIRST_ROW_Y    (STATUS_BAR_H + 4)

#define BLINK_PERIOD   400  /* ms */
#define BAT_LEVELS     5

typedef struct {
    const lv_img_dsc_t *icon;       /* 50px normal */
    const lv_img_dsc_t *icon_big;   /* 60px focused */
    page_id_t           page_id;
} menu_item_t;

static const lv_img_dsc_t *bat_icons[BAT_LEVELS] = {
    &icon_bat0, &icon_bat1, &icon_bat2, &icon_bat3, &icon_bat4
};

static const menu_item_t menu_items[MENU_COUNT] = {
    { &icon_voice_gen,  &icon_voice_gen_big,  PAGE_VOICE_GEN  },
    { &icon_photo_text, &icon_photo_text_big, PAGE_PHOTO_TEXT },
    { &icon_recognize,  &icon_recognize_big,  PAGE_RECOGNIZE  },
    { &icon_ai_chat,    &icon_ai_chat_big,    PAGE_AI_CHAT    },
    { &icon_camera,     &icon_camera_big,     PAGE_CAMERA     },
    { &icon_video,      &icon_video_big,      PAGE_VIDEO      },
    { &icon_album,      &icon_album_big,      PAGE_ALBUM      },
    { &icon_settings,   &icon_settings_big,   PAGE_SETTINGS_LIST },
};

/* Row Y positions computed dynamically */

/* ========== Module State ========== */

static lv_obj_t *cells[MENU_COUNT];
static lv_obj_t *imgs[MENU_COUNT];
static lv_timer_t *blink_timer = NULL;
static uint32_t blink_user_data = 0;  /* same pattern as Djj project */
static int focused_idx = 0;
static uint8_t blink_state = 0;

/* --- Startup popup state --- */
enum { POPUP_NONE, POPUP_AI_CONNECTING, POPUP_WIFI_CONFIG, POPUP_AI_FAIL };
static uint8_t  g_popup_state = POPUP_NONE;
static lv_obj_t *g_popup_overlay = NULL;
static lv_obj_t *g_popup_box = NULL;
static lv_obj_t *g_popup_label1 = NULL;
static lv_obj_t *g_popup_highlight = NULL;
static lv_obj_t *g_popup_btn_labels[2] = {NULL, NULL};
static int8_t   g_popup_btn_idx = 0;
static lv_timer_t *g_popup_timer = NULL;
static uint8_t  g_popup_tick = 0;
static uint8_t  g_startup_done = 0;

/* ========== Blink Timer ========== */

static void blink_cb(lv_timer_t *timer)
{
    blink_state = !blink_state;

    /* 直接使用 focused_idx 作为闪烁目标，避免 group focus 不同步 */
    if (focused_idx >= 0 && focused_idx < MENU_COUNT) {
        lv_img_set_src(imgs[focused_idx],
            blink_state ? menu_items[focused_idx].icon_big
                        : menu_items[focused_idx].icon);
    }
}

/* ========== Focus & Key Handlers ========== */

static void focus_changed_cb(lv_group_t *group)
{
    lv_obj_t *new_focus = lv_group_get_focused(group);
    /* Restore old focus to small icon */
    if (focused_idx >= 0 && focused_idx < MENU_COUNT) {
        lv_img_set_src(imgs[focused_idx], menu_items[focused_idx].icon);
    }
    /* Find new focus */
    focused_idx = -1;
    for (int i = 0; i < MENU_COUNT; i++) {
        if (cells[i] == new_focus) {
            focused_idx = i;
            break;
        }
    }
    blink_state = 0;
}

static void cell_key_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_KEY) return;

    uint32_t key = lv_event_get_key(e);
    if (key != LV_KEY_ENTER) return;

    lv_obj_t *focused = lv_group_get_focused(ui_manager_group());
    for (int i = 0; i < MENU_COUNT; i++) {
        if (cells[i] == focused) {
            os_printf("[menu] select item %d -> page %d\r\n", i, menu_items[i].page_id);
            ui_manager_push(menu_items[i].page_id);
            return;
        }
    }
}

/* ========== Status Bar ========== */

static lv_obj_t *status_time_label;
static lv_obj_t *status_bat_img;
static lv_obj_t *status_wifi_img;

static void create_status_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), STATUS_BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(bar, LV_OPA_30, 0);

    status_bat_img = lv_img_create(bar);
    {
        enum bat_level lvl = app_battery_get_level();
        lv_img_set_src(status_bat_img, (lvl >= 0 && lvl < BAT_LEVELS) ? bat_icons[lvl] : bat_icons[BAT_LEVEL_HIGH]);
    }
    lv_obj_align(status_bat_img, LV_ALIGN_LEFT_MID, 2, 0);

    status_time_label = lv_label_create(bar);
    lv_label_set_text(status_time_label, "00:00");
    lv_obj_align(status_time_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(status_time_label, lv_color_hex(0x333333), 0);

    status_wifi_img = lv_img_create(bar);
    lv_img_set_src(status_wifi_img, g_wifi_connected ? &icon_wifi : &icon_no_wifi);
    lv_obj_align(status_wifi_img, LV_ALIGN_RIGHT_MID, -4, 0);

    /* Register widgets for real-time status bar updates */
    ui_manager_set_bat_img(status_bat_img);
    ui_manager_set_wifi_img(status_wifi_img);
    ui_manager_set_time_label(status_time_label);
}

void ui_status_bar_update_bat(uint32_t mv)
{
    if (status_bat_img) {
        enum bat_level lvl = app_battery_get_level();
        if (lvl >= 0 && lvl < BAT_LEVELS) {
            lv_img_set_src(status_bat_img, bat_icons[lvl]);
        }
    }
}

void ui_status_bar_update_wifi(int connected)
{
    if (status_wifi_img) {
        lv_img_set_src(status_wifi_img, connected ? &icon_wifi : &icon_no_wifi);
    }
}

void ui_status_bar_update_time(const char *str)
{
    if (status_time_label && str) {
        lv_label_set_text(status_time_label, str);
    }
}

/* ========== Startup Popup ========== */

static void startup_popup_destroy(void)
{
    if (g_popup_timer) { lv_timer_del(g_popup_timer); g_popup_timer = NULL; }
    if (g_popup_overlay) { lv_obj_del(g_popup_overlay); g_popup_overlay = NULL; }
    g_popup_box = NULL;
    g_popup_label1 = NULL;
    g_popup_highlight = NULL;
    g_popup_btn_labels[0] = NULL;
    g_popup_btn_labels[1] = NULL;
    g_popup_state = POPUP_NONE;
}

static void popup_config_update_highlight(void)
{
    if (!g_popup_highlight) return;
    lv_obj_set_pos(g_popup_highlight, g_popup_btn_idx == 0 ? 16 : 101, 50);
    for (int i = 0; i < 2; i++) {
        if (g_popup_btn_labels[i])
            lv_obj_set_style_text_color(g_popup_btn_labels[i],
                i == g_popup_btn_idx ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000), 0);
    }
}

static void popup_timer_cb(lv_timer_t *t)
{
    if (g_popup_state != POPUP_AI_CONNECTING) return;
    g_popup_tick++;
    if (is_brtc_running()) {
        startup_popup_destroy();
        return;
    }
    if (g_popup_tick >= 10) { /* 500ms * 10 = 5s */
        lv_timer_del(g_popup_timer);
        g_popup_timer = NULL;
        g_popup_state = POPUP_AI_FAIL;
        if (g_popup_label1) {
            lv_label_set_text(g_popup_label1, "AI\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5"); /* AI连接失败 */
            lv_obj_align(g_popup_label1, LV_ALIGN_TOP_MID, 0, 10);
        }
        lv_obj_t *lbl2 = lv_label_create(g_popup_box);
        lv_label_set_text(lbl2, "\xe8\xaf\xb7\xe6\xa3\x80\xe6\x9f\xa5\xe7\xbd\x91\xe7\xbb\x9c"); /* 请检查网络 */
        lv_obj_align(lbl2, LV_ALIGN_CENTER, 0, 2);
        lv_obj_set_style_text_color(lbl2, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(lbl2, POPUP_FONT, 0);
        lv_obj_t *hint = lv_label_create(g_popup_box);
        lv_label_set_text(hint, "OK\xe7\xa1\xae\xe8\xae\xa4"); /* OK确认 */
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x999999), 0);
        lv_obj_set_style_text_font(hint, POPUP_FONT, 0);
    }
}

static void startup_popup_show(lv_obj_t *root)
{
    if (g_startup_done) return;
    g_startup_done = 1;

    /* Semi-transparent overlay */
    g_popup_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(g_popup_overlay);
    lv_obj_set_size(g_popup_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_popup_overlay, 0, 0);
    lv_obj_set_style_bg_color(g_popup_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_popup_overlay, LV_OPA_50, 0);
    lv_obj_clear_flag(g_popup_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* White rounded box */
    g_popup_box = lv_obj_create(g_popup_overlay);
    lv_obj_remove_style_all(g_popup_box);
    lv_obj_set_size(g_popup_box, 200, 100);
    lv_obj_align(g_popup_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(g_popup_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_popup_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_popup_box, 10, 0);
    lv_obj_clear_flag(g_popup_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    if (g_wifi_connected) {
        g_popup_state = POPUP_AI_CONNECTING;
        g_popup_tick = 0;
        g_popup_label1 = lv_label_create(g_popup_box);
        lv_label_set_text(g_popup_label1, "AI\xe8\xbf\x9e\xe6\x8e\xa5\xe4\xb8\xad..."); /* AI连接中... */
        lv_obj_align(g_popup_label1, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_color(g_popup_label1, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_font(g_popup_label1, POPUP_FONT, 0);
        g_popup_timer = lv_timer_create(popup_timer_cb, 500, NULL);
    } else {
        g_popup_state = POPUP_WIFI_CONFIG;
        g_popup_btn_idx = 0;
        g_popup_label1 = lv_label_create(g_popup_box);
        lv_label_set_text(g_popup_label1, "\xe8\xaf\xb7\xe5\x85\x88\xe9\x85\x8d\xe7\xbd\xaeWiFi"); /* 请先配置WiFi */
        lv_obj_align(g_popup_label1, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_set_style_text_color(g_popup_label1, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_font(g_popup_label1, POPUP_FONT, 0);

        g_popup_highlight = lv_obj_create(g_popup_box);
        lv_obj_remove_style_all(g_popup_highlight);
        lv_obj_set_size(g_popup_highlight, 75, 28);
        lv_obj_set_style_bg_color(g_popup_highlight, lv_color_hex(0x1890FF), 0);
        lv_obj_set_style_bg_opa(g_popup_highlight, LV_OPA_80, 0);
        lv_obj_set_style_radius(g_popup_highlight, 6, 0);
        lv_obj_clear_flag(g_popup_highlight, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        g_popup_btn_labels[0] = lv_label_create(g_popup_box);
        lv_label_set_text(g_popup_btn_labels[0], "\xe9\x85\x8d\xe7\xbd\xae"); /* 配置 */
        lv_obj_set_pos(g_popup_btn_labels[0], 42, 56);
        lv_obj_set_style_text_color(g_popup_btn_labels[0], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(g_popup_btn_labels[0], POPUP_FONT, 0);

        g_popup_btn_labels[1] = lv_label_create(g_popup_box);
        lv_label_set_text(g_popup_btn_labels[1], "\xe6\x94\xbe\xe5\xbc\x83"); /* 放弃 */
        lv_obj_set_pos(g_popup_btn_labels[1], 130, 56);
        lv_obj_set_style_text_color(g_popup_btn_labels[1], lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_font(g_popup_btn_labels[1], POPUP_FONT, 0);

        popup_config_update_highlight();
    }
}

/* ========== Page Callbacks ========== */

static lv_obj_t *main_menu_create(ui_page_t *page, lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* Background color (wallpaper will be regenerated for 240x320 later) */
    lv_obj_t *bg = lv_img_create(root);
    lv_img_set_src(bg, &Wallpaper0);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    create_status_bar(root);

    lv_group_t *grp = ui_manager_group();

    /* 保存上次选中位置到局部变量，防止循环中意外覆盖 */
    int saved_focus = focused_idx;
    if (saved_focus < 0 || saved_focus >= MENU_COUNT) saved_focus = 0;

    /* Clear stale pointers before adding objects */
    os_memset(cells, 0, sizeof(cells));
    os_memset(imgs, 0, sizeof(imgs));

    for (int i = 0; i < MENU_COUNT; i++) {
        int col = i % MENU_COLS;
        int row = i / MENU_COLS;

        int x = col * (CELL_W + COL_GAP);
        int y = FIRST_ROW_Y + row * (CELL_H + ROW_GAP);

        lv_obj_t *cell = lv_obj_create(root);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, CELL_W, CELL_H);
        lv_obj_set_pos(cell, x, y);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICK_FOCUSABLE);

        lv_obj_t *img = lv_img_create(cell);
        lv_img_set_src(img, menu_items[i].icon);
        lv_obj_center(img);

        lv_obj_add_event_cb(cell, cell_key_handler, LV_EVENT_KEY, NULL);

        cells[i] = cell;
        imgs[i] = img;
        lv_group_add_obj(grp, cell);
    }

    /* 从局部变量恢复焦点 */
    focused_idx = saved_focus;
    lv_group_focus_obj(cells[focused_idx]);

    /* 注册 focus callback（在焦点恢复之后）*/
    lv_group_set_focus_cb(grp, focus_changed_cb);

    /* Create blink timer — same pattern as Djj project */
    if (!blink_timer) {
        blink_timer = lv_timer_create(blink_cb, BLINK_PERIOD, &blink_user_data);
    }
    startup_popup_show(root);

    os_printf("[menu] created, focus=%d\r\n", focused_idx);
    return root;
}

static void main_menu_destroy(ui_page_t *page)
{
    if (blink_timer) {
        lv_timer_del(blink_timer);
        blink_timer = NULL;
    }
    if (g_popup_timer) { lv_timer_del(g_popup_timer); g_popup_timer = NULL; }
    g_popup_state = POPUP_NONE;
    g_popup_overlay = NULL;
    g_popup_box = NULL;
    g_popup_label1 = NULL;
    g_popup_highlight = NULL;
    g_popup_btn_labels[0] = NULL;
    g_popup_btn_labels[1] = NULL;
    status_bat_img = NULL;
    status_time_label = NULL;
    status_wifi_img = NULL;
    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
}

static void main_menu_on_key(ui_page_t *page, key_id_t id, key_event_t evt)
{
    /* Popup key interception */
    if (g_popup_state == POPUP_WIFI_CONFIG) {
        if (evt != KEY_EVT_SHORT) return;
        if (id == KEY_ID_UP || id == KEY_ID_DOWN) {
            g_popup_btn_idx = !g_popup_btn_idx;
            popup_config_update_highlight();
        } else if (id == KEY_ID_OK) {
            if (g_popup_btn_idx == 0) {
                startup_popup_destroy();
                ui_manager_push(PAGE_WIFI_SCAN);
            } else {
                startup_popup_destroy();
            }
        } else if (id == KEY_ID_M) {
            startup_popup_destroy();
        }
        return;
    }
    if (g_popup_state == POPUP_AI_FAIL) {
        if (evt != KEY_EVT_SHORT) return;
        startup_popup_destroy();
        return;
    }
    if (g_popup_state == POPUP_AI_CONNECTING) {
        return;
    }

    if (evt != KEY_EVT_SHORT) return;

    os_printf("[menu] on_key id=%d focused=%d\r\n", id, focused_idx);

    switch (id) {
    case KEY_ID_M:
        break;
    case KEY_ID_OK:
        if (focused_idx >= 0 && focused_idx < MENU_COUNT) {
            os_printf("[menu] select item %d -> page %d\r\n", focused_idx, menu_items[focused_idx].page_id);
            ui_manager_push(menu_items[focused_idx].page_id);
        }
        break;
    case KEY_ID_UP:
        /* UP = previous item (natural portrait direction) */
        if (focused_idx > 0) focused_idx--;
        else focused_idx = MENU_COUNT - 1;
        os_printf("[menu] UP -> focus %d cell=%p\r\n", focused_idx, cells[focused_idx]);
        lv_group_focus_obj(cells[focused_idx]);
        break;
    case KEY_ID_DOWN:
        /* DOWN = next item (natural portrait direction) */
        if (focused_idx < MENU_COUNT - 1) focused_idx++;
        else focused_idx = 0;
        os_printf("[menu] DOWN -> focus %d cell=%p\r\n", focused_idx, cells[focused_idx]);
        lv_group_focus_obj(cells[focused_idx]);
        break;
    default:
        break;
    }
}

/* ========== Page Instance ========== */

ui_page_t main_menu_page = {
    .id       = PAGE_MAIN_MENU,
    .create   = main_menu_create,
    .destroy  = main_menu_destroy,
    .on_key   = main_menu_on_key,
    .root_obj = NULL,
};
