#ifndef CONFIG_H
#define CONFIG_H

#include <json-glib/json-glib.h>
#include <stdbool.h>

typedef struct {
    char *api_key;
    char *download_dir;

    /* filters */
    bool  cat_general;
    bool  cat_anime;
    bool  cat_people;
    bool  pur_sfw;
    bool  pur_sketchy;
    bool  pur_nsfw;
    int   sort_index;       /* 0-5 */
    int   top_range_index;  /* 0-6 */
    int   ratio_index;      /* 0-12 */
    char *min_resolution;
    int   wallpaper_method; /* 0=auto, 1=gnome, 2=kde, 3=hyprpaper */
    int   gsk_renderer;    /* 0=cairo, 1=vulkan */
} AppConfig;

AppConfig *config_load(void);
void       config_save(const AppConfig *cfg);
void       config_free(AppConfig *cfg);

#endif
