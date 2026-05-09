#ifndef UI_MAIN_MENU_H
#define UI_MAIN_MENU_H

#include "ui_manager.h"

extern ui_page_t main_menu_page;

/* Status bar update APIs (call from main.c or power/wifi modules) */
void ui_status_bar_update_bat(uint32_t mv);
void ui_status_bar_update_wifi(int connected);
void ui_status_bar_update_time(const char *str);

#endif /* UI_MAIN_MENU_H */
