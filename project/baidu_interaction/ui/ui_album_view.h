#ifndef UI_ALBUM_VIEW_H
#define UI_ALBUM_VIEW_H

#include "ui_manager.h"

void ui_album_view_register(void);

/* Set target file before pushing PAGE_ALBUM_VIEW */
void ui_album_view_set_file(const char *folder_path, const char *filename,
                            uint16_t file_index, uint16_t total_files);

#endif /* UI_ALBUM_VIEW_H */
