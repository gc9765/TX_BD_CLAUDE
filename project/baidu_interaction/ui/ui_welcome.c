#include "sys_config.h"
#include "typesdef.h"
#include "lvgl/lvgl.h"
#include "ui_manager.h"
#include "ui_welcome.h"
#include "../res/res_icons.h"

#define WELCOME_DURATION_MS  2000

static lv_timer_t *welcome_timer = NULL;

static void welcome_timer_cb(lv_timer_t *timer)
{
    if (welcome_timer) {
        lv_timer_del(welcome_timer);
        welcome_timer = NULL;
    }
    ui_manager_pop();
}

static lv_obj_t *welcome_create(ui_page_t *page, lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* Centered welcome logo */
    lv_obj_t *img = lv_img_create(root);
    lv_img_set_src(img, &poweron_welcome_200);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);

    /* Auto-switch to main menu after delay */
    welcome_timer = lv_timer_create(welcome_timer_cb, WELCOME_DURATION_MS, NULL);
    lv_timer_set_repeat_count(welcome_timer, 1);

    os_printf("[welcome] created\r\n");
    return root;
}

static void welcome_destroy(ui_page_t *page)
{
    if (welcome_timer) {
        lv_timer_del(welcome_timer);
        welcome_timer = NULL;
    }
    if (page->root_obj) {
        lv_obj_del(page->root_obj);
        page->root_obj = NULL;
    }
}

ui_page_t welcome_page = {
    .id       = PAGE_WELCOME,
    .create   = welcome_create,
    .destroy  = welcome_destroy,
    .on_key   = NULL,
    .root_obj = NULL,
};
