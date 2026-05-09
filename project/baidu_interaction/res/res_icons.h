#ifndef RES_ICONS_H
#define RES_ICONS_H

#include "lvgl/lvgl.h"

/* Menu icons 50x50 normal (res_icon50.c) */
extern const lv_img_dsc_t icon_voice_gen;
extern const lv_img_dsc_t icon_photo_text;
extern const lv_img_dsc_t icon_recognize;
extern const lv_img_dsc_t icon_ai_chat;
extern const lv_img_dsc_t icon_camera;
extern const lv_img_dsc_t icon_video;
extern const lv_img_dsc_t icon_album;
extern const lv_img_dsc_t icon_settings;

/* Menu icons 60x60 focused blink (res_icon60.c) */
extern const lv_img_dsc_t icon_voice_gen_big;
extern const lv_img_dsc_t icon_photo_text_big;
extern const lv_img_dsc_t icon_recognize_big;
extern const lv_img_dsc_t icon_ai_chat_big;
extern const lv_img_dsc_t icon_camera_big;
extern const lv_img_dsc_t icon_video_big;
extern const lv_img_dsc_t icon_album_big;
extern const lv_img_dsc_t icon_settings_big;

/* Battery icons 32x28, 5 levels (res_bat.c) */
extern const lv_img_dsc_t icon_bat0;  /* full   >= 4.2V */
extern const lv_img_dsc_t icon_bat1;  /* high   >= 3.7V */
extern const lv_img_dsc_t icon_bat2;  /* medium >= 3.4V */
extern const lv_img_dsc_t icon_bat3;  /* low    >= 3.0V */
extern const lv_img_dsc_t icon_bat4;  /* critical < 3.0V */

/* WiFi icons 26x26 (res_wifi.c) */
extern const lv_img_dsc_t icon_wifi;
extern const lv_img_dsc_t icon_no_wifi;

/* Wallpaper background 240x320 portrait (res_wallpaper.c) */
extern const lv_img_dsc_t Wallpaper0;

/* Welcome logo 200x200 (res_welcome.c) */
extern const lv_img_dsc_t poweron_welcome_200;

/* Folder icon 60x60 focused (res_folder_icon.c) */
extern const lv_img_dsc_t folder_icon_60;

/* Folder icon 50x50 normal (res_folder_icon_50.c) */
extern const lv_img_dsc_t folder_icon_50;

/* Waiting circle 80x80 with alpha (res_waiting_circle.c) */
extern const lv_img_dsc_t power_circle_80;

/* Delete icon 18x18 (res_delete_icon.c) */
extern const lv_img_dsc_t delete_18x18;

/* Character icons (res_character_icons.c) */
extern const lv_img_dsc_t chinesepeople1_110;
extern const lv_img_dsc_t chinesepeople1_200;
extern const lv_img_dsc_t Doctor1_110;
extern const lv_img_dsc_t Doctor1_200;
extern const lv_img_dsc_t friendship_day_110;
extern const lv_img_dsc_t friendship_day_200;
extern const lv_img_dsc_t smart_robot_110;
extern const lv_img_dsc_t smart_robot_200;

/* Arrow icon 40x40 (res_arrow_next.c) */
extern const lv_img_dsc_t next_40x40;

#endif /* RES_ICONS_H */
