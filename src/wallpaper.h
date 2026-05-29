#ifndef WALLPAPER_H
#define WALLPAPER_H

#include "api.h"

bool wallpaper_download(WallpaperInfo *info, const char *dest_dir);
char *wallpaper_get_path(WallpaperInfo *info, const char *dest_dir);
void wallpaper_set_as_background(const char *path);
void wallpaper_copy_to_clipboard(const char *path);

#endif
