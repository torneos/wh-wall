#ifndef API_H
#define API_H

#include "config.h"
#include <json-glib/json-glib.h>
#include <stdbool.h>

typedef struct {
    char *id;
    char *url;
    char *thumb_url;
    char *resolution;
    char *category;
    char *purity;
    int  favorites;
    int  views;
} WallpaperInfo;

void        wallpaper_info_free(WallpaperInfo *info);
void        api_set_config(AppConfig *cfg);
JsonArray  *api_search(const char *query, int page, const char *categories,
                       const char *purity, const char *sorting, const char *top_range,
                       const char *ratios, const char *atleast,
                       int *out_last_page);
char       *api_get_api_key(void);
bool        api_has_api_key(void);
char       *api_get_username(void);

#endif
