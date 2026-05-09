#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "lvgl/lvgl.h"
#include "ui_manager.h"
#include "../app_power.h"
#include "../res/res_icons.h"
#include "../app_wifi_config.h"

#define PAGE_STACK_MAX  4

/* Key event queue — allows deferring key processing to gui_thread */
#define KEY_QUEUE_SIZE  8
static struct {
    key_id_t    id;
    key_event_t evt;
} key_queue_buf[KEY_QUEUE_SIZE];
static volatile int key_queue_head = 0;
static int key_queue_tail = 0;
static lv_timer_t *key_process_timer = NULL;

static ui_page_t *page_registry[PAGE_COUNT];
static page_id_t  page_stack[PAGE_STACK_MAX];
static uint8_t    stack_top = 0;
static lv_obj_t  *screen_container = NULL;
static lv_group_t *nav_group = NULL;
static ui_page_t *current_page = NULL;

page_id_t ui_manager_current_page(void)
{
    return current_page ? current_page->id : PAGE_NONE;
}

/* Status bar widget pointers — set by each page, updated by status_timer */
static lv_obj_t *cur_bat_img  = NULL;
static lv_obj_t *cur_wifi_img = NULL;
static lv_obj_t *cur_time_lbl = NULL;
static lv_timer_t *status_timer = NULL;

#define BAT_LEVELS     5
/* BAT_LEVEL_FULL=0 → 满电图标, BAT_LEVEL_CRITICAL=4 → 空图标 */
static const lv_img_dsc_t *bat_icons[BAT_LEVELS] = {
    &icon_bat4, &icon_bat3, &icon_bat2, &icon_bat1, &icon_bat0
};

/* ========== Status Bar Registration ========== */

void ui_manager_set_bat_img(lv_obj_t *img)   { cur_bat_img  = img; }
void ui_manager_set_wifi_img(lv_obj_t *img)  { cur_wifi_img = img; }
void ui_manager_set_time_label(lv_obj_t *lbl) { cur_time_lbl = lbl; }

static void status_timer_cb(lv_timer_t *timer)
{
    /* Battery icon */
    if (cur_bat_img) {
        enum bat_level lvl = app_battery_get_level();
        if (lvl >= 0 && lvl < BAT_LEVELS) {
            lv_img_set_src(cur_bat_img, bat_icons[lvl]);
        }
    }

    /* WiFi icon */
    if (cur_wifi_img) {
        lv_img_set_src(cur_wifi_img, g_wifi_connected ? &icon_wifi : &icon_no_wifi);
    }

    /* Time display (HH:MM, UTC+8) */
    if (cur_time_lbl) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        tv.tv_sec += 8 * 3600;  /* UTC+8 Beijing time */
        struct tm *t = localtime(&tv.tv_sec);
        if (t && (t->tm_hour > 0 || t->tm_min > 0)) {
            char buf[8];
            os_snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
            lv_label_set_text(cur_time_lbl, buf);
        }
    }
}

/* ========== Page Registration ========== */

void ui_page_register(ui_page_t *page)
{
    if (page && page->id > PAGE_NONE && page->id < PAGE_COUNT) {
        page_registry[page->id] = page;
    }
}

/* ========== Key Event Queue ========== */

/*
 * Keys arrive from the key-scan task (app_key_callback → on_key_event).
 * LVGL is NOT thread-safe, so we must NOT touch LVGL objects outside
 * gui_thread.  We buffer events here and drain the buffer from an LVGL
 * timer which runs inside gui_thread.
 */

static void key_process_cb(lv_timer_t *timer)
{
    while (key_queue_tail != key_queue_head) {
        key_id_t    id  = key_queue_buf[key_queue_tail].id;
        key_event_t evt = key_queue_buf[key_queue_tail].evt;
        key_queue_tail = (key_queue_tail + 1) % KEY_QUEUE_SIZE;

        if (current_page && current_page->on_key) {
            current_page->on_key(current_page, id, evt);
        }
    }
}

/* ========== Internal Helpers ========== */

static void switch_to_page(page_id_t id)
{
    ui_page_t *page = page_registry[id];
    if (!page) {
        os_printf("[ui_mgr] page %d not registered\r\n", id);
        return;
    }

    /* Clear focus callback BEFORE destroying the page.
     * When lv_obj_del removes focused cells from the group, LVGL
     * refocuses the next object.  If the old callback (e.g.
     * focus_changed_cb in ui_main_menu.c) is still active, it
     * overwrites focused_idx on every refocus, corrupting the
     * saved position.  Clearing it first prevents this. */
    lv_group_set_focus_cb(nav_group, NULL);

    /* Destroy current page */
    if (current_page) {
        os_printf("[ui_mgr] destroy page %d\r\n", current_page->id);
        if (current_page->destroy) {
            current_page->destroy(current_page);
        }
        current_page->root_obj = NULL;
    }

    /* Create new page */
    os_printf("[ui_mgr] create page %d\r\n", id);
    cur_bat_img  = NULL;
    cur_wifi_img = NULL;
    cur_time_lbl = NULL;
    page->root_obj = page->create(page, screen_container);
    current_page = page;

    /* 立即刷新状态栏（电池/时间/WiFi），避免页面切换时显示 00:00 */
    status_timer_cb(NULL);

    os_printf("[ui_mgr] switch to page %d\r\n", id);
}

/* ========== Manager API ========== */

extern lv_indev_t *indev_keypad;

void ui_manager_init(void)
{
    memset(page_registry, 0, sizeof(page_registry));
    memset(page_stack, 0, sizeof(page_stack));
    stack_top = 0;

    /* Print LVGL screen resolution */
    lv_coord_t w = lv_disp_get_hor_res(NULL);
    lv_coord_t h = lv_disp_get_ver_res(NULL);
    os_printf("[ui_mgr] screen resolution: %dx%d\r\n", w, h);

    /* Reuse group created by main_ui() and bound to indev_keypad */
    nav_group = lv_group_get_default();
    if (!nav_group) {
        nav_group = lv_group_create();
        lv_group_set_default(nav_group);
    }
    lv_group_set_wrap(nav_group, 1);
    lv_group_remove_all_objs(nav_group);

    /* Ensure indev_keypad uses our group */
    if (indev_keypad) {
        lv_indev_set_group(indev_keypad, nav_group);
    }

    /* Create transparent fullscreen container */
    screen_container = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(screen_container);
    lv_obj_set_size(screen_container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(screen_container, 0, 0);
    lv_obj_clear_flag(screen_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Register pages (declared in their respective headers) */
    extern ui_page_t welcome_page;
    extern ui_page_t main_menu_page;
    extern ui_page_t settings_list_page;
    extern ui_page_t wifi_scan_page;
    extern void ui_placeholder_register_all(void);
    extern void ui_camera_register(void);
    extern void ui_ocr_register(void);
    extern void ui_ai_chat_register(void);
    extern void ui_recorder_register(void);
    extern void ui_album_register(void);
    extern void ui_album_grid_register(void);
    extern void ui_album_view_register(void);

    ui_page_register(&welcome_page);
    ui_page_register(&main_menu_page);
    ui_page_register(&settings_list_page);
    ui_page_register(&wifi_scan_page);
    ui_placeholder_register_all();
    ui_camera_register();
    ui_ocr_register();
    ui_ai_chat_register();
    ui_recorder_register();
    ui_album_register();
    ui_album_grid_register();
    ui_album_view_register();

    /* Start key-processing timer (drains queue inside gui_thread) */
    key_process_timer = lv_timer_create(key_process_cb, 10, NULL);
    lv_timer_set_repeat_count(key_process_timer, -1);

    /* Start status bar refresh timer (battery, wifi icon) */
    status_timer = lv_timer_create(status_timer_cb, 2000, NULL);
    lv_timer_set_repeat_count(status_timer, -1);

    /* Push welcome page first, it auto-transitions to main menu */
    page_stack[stack_top++] = PAGE_MAIN_MENU;
    page_stack[stack_top++] = PAGE_WELCOME;
    switch_to_page(PAGE_WELCOME);
}

void ui_manager_push(page_id_t id)
{
    if (stack_top >= PAGE_STACK_MAX) {
        os_printf("[ui_mgr] stack overflow\r\n");
        return;
    }

    /* switch_to_page will destroy old page (auto-removes from group),
     * then create new page (adds new objs to group). No need to
     * manually lv_group_remove_all_objs — lv_obj_del handles it. */
    page_stack[stack_top++] = id;
    switch_to_page(id);
}

void ui_manager_pop(void)
{
    if (stack_top <= 1) {
        os_printf("[ui_mgr] can't pop root\r\n");
        return;
    }

    stack_top--;
    page_id_t prev_id = page_stack[stack_top - 1];

    switch_to_page(prev_id);
}

void ui_manager_handle_key(key_id_t id, key_event_t evt)
{
    /* Queue the event — actual processing happens in gui_thread via
     * key_process_timer, so LVGL objects are never touched from the
     * key-scan task. */
    int next = (key_queue_head + 1) % KEY_QUEUE_SIZE;
    if (next != key_queue_tail) {
        key_queue_buf[key_queue_head].id  = id;
        key_queue_buf[key_queue_head].evt = evt;
        key_queue_head = next;
    }
}

lv_group_t *ui_manager_group(void)
{
    return nav_group;
}
