#ifndef UI_ALBUM_GRID_H
#define UI_ALBUM_GRID_H

#include "ui_manager.h"

void ui_album_grid_register(void);

/* Set target folder before pushing PAGE_ALBUM_GRID */
void ui_album_grid_set_folder(const char *path);

#endif /* UI_ALBUM_GRID_H */
