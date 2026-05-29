#ifndef WALLPAPER_H
#define WALLPAPER_H

#include "api.h"
#include <gtk/gtk.h>

bool wallpaper_download(WallpaperInfo *info, const char *dest_dir,
                         GtkProgressBar *progress);
char *wallpaper_get_path(WallpaperInfo *info, const char *dest_dir);
void wallpaper_set_as_background(const char *path, int method);
void wallpaper_copy_to_clipboard(const char *path);

#endif
