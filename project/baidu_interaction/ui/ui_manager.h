#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "typesdef.h"
#include "lvgl/lvgl.h"
#include "../app_key.h"

/* Page identifiers */
typedef enum {
    PAGE_NONE = 0,
    PAGE_WELCOME,
    PAGE_MAIN_MENU,
    PAGE_VOICE_GEN,
    PAGE_PHOTO_TEXT,
    PAGE_RECOGNIZE,
    PAGE_AI_CHAT,
    PAGE_CAMERA,
    PAGE_VIDEO,
    PAGE_ALBUM,
    PAGE_ALBUM_GRID,
    PAGE_ALBUM_VIEW,
    PAGE_SETTINGS,
    PAGE_SETTINGS_LIST,
    PAGE_WIFI_SCAN,
    PAGE_COUNT
} page_id_t;

/* Page lifecycle interface */
typedef struct ui_page ui_page_t;

struct ui_page {
    page_id_t id;
    lv_obj_t *(*create)(ui_page_t *page, lv_obj_t *parent);
    void (*destroy)(ui_page_t *page);
    void (*on_key)(ui_page_t *page, key_id_t id, key_event_t evt);
    lv_obj_t *root_obj;
};

/* Manager API */
void ui_manager_init(void);
void ui_manager_push(page_id_t id);
void ui_manager_pop(void);
void ui_manager_handle_key(key_id_t id, key_event_t evt);
lv_group_t *ui_manager_group(void);

/* Status bar widget registration for real-time updates */
void ui_manager_set_bat_img(lv_obj_t *img);
void ui_manager_set_wifi_img(lv_obj_t *img);
void ui_manager_set_time_label(lv_obj_t *lbl);

/* Page registration */
void ui_page_register(ui_page_t *page);

/* Query current page */
page_id_t ui_manager_current_page(void);

#endif /* UI_MANAGER_H */
