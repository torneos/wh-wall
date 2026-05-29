#include "config.h"
#include "log.h"
#include <string.h>

#define CONFIG_PATH_BUILDER() \
    g_build_filename(g_get_user_config_dir(), "wh-wall", "config.json", NULL)

static void
config_set_defaults(AppConfig *cfg)
{
    cfg->cat_general     = TRUE;
    cfg->cat_anime       = TRUE;
    cfg->cat_people      = TRUE;
    cfg->pur_sfw         = TRUE;
    cfg->pur_sketchy     = FALSE;
    cfg->pur_nsfw        = FALSE;
    cfg->sort_index      = 0;   /* date_added */
    cfg->top_range_index = 2;   /* 1w */
    cfg->ratio_index     = 0;   /* Any */
    cfg->min_resolution  = NULL;
    cfg->api_key         = NULL;
    cfg->download_dir    = NULL;
    cfg->wallpaper_method = 0;  /* auto */
}

AppConfig *
config_load(void)
{
    AppConfig *cfg = g_new0(AppConfig, 1);
    config_set_defaults(cfg);

    char *path = CONFIG_PATH_BUILDER();
    char *raw = NULL;
    gsize len = 0;

    if (!g_file_get_contents(path, &raw, &len, NULL) || len == 0) {
        log_info("No config file at %s, using defaults", path);
        g_free(raw);
        g_free(path);
        return cfg;
    }
    g_free(path);

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, raw, len, NULL)) {
        log_warn("Failed to parse config JSON, using defaults");
        g_object_unref(parser);
        g_free(raw);
        return cfg;
    }
    g_free(raw);

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = root ? json_node_get_object(root) : NULL;
    if (!obj) {
        g_object_unref(parser);
        return cfg;
    }

    if (json_object_has_member(obj, "api_key"))
        cfg->api_key = g_strdup(json_object_get_string_member(obj, "api_key"));
    if (json_object_has_member(obj, "download_dir"))
        cfg->download_dir = g_strdup(json_object_get_string_member(obj, "download_dir"));

    if (json_object_has_member(obj, "cat_general"))
        cfg->cat_general = json_object_get_boolean_member(obj, "cat_general");
    if (json_object_has_member(obj, "cat_anime"))
        cfg->cat_anime = json_object_get_boolean_member(obj, "cat_anime");
    if (json_object_has_member(obj, "cat_people"))
        cfg->cat_people = json_object_get_boolean_member(obj, "cat_people");
    if (json_object_has_member(obj, "pur_sfw"))
        cfg->pur_sfw = json_object_get_boolean_member(obj, "pur_sfw");
    if (json_object_has_member(obj, "pur_sketchy"))
        cfg->pur_sketchy = json_object_get_boolean_member(obj, "pur_sketchy");
    if (json_object_has_member(obj, "pur_nsfw"))
        cfg->pur_nsfw = json_object_get_boolean_member(obj, "pur_nsfw");
    if (json_object_has_member(obj, "sort_index"))
        cfg->sort_index = (int)json_object_get_int_member(obj, "sort_index");
    if (json_object_has_member(obj, "top_range_index"))
        cfg->top_range_index = (int)json_object_get_int_member(obj, "top_range_index");
    if (json_object_has_member(obj, "ratio_index"))
        cfg->ratio_index = (int)json_object_get_int_member(obj, "ratio_index");
    if (json_object_has_member(obj, "min_resolution"))
        cfg->min_resolution = g_strdup(json_object_get_string_member(obj, "min_resolution"));
    if (json_object_has_member(obj, "wallpaper_method"))
        cfg->wallpaper_method = (int)json_object_get_int_member(obj, "wallpaper_method");

    g_object_unref(parser);
    log_info("Config loaded from config.json");
    return cfg;
}

void
config_save(const AppConfig *cfg)
{
    if (!cfg) return;

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);

    if (cfg->api_key)
        json_builder_set_member_name(builder, "api_key"),
        json_builder_add_string_value(builder, cfg->api_key);
    if (cfg->download_dir)
        json_builder_set_member_name(builder, "download_dir"),
        json_builder_add_string_value(builder, cfg->download_dir);

    json_builder_set_member_name(builder, "cat_general"),
    json_builder_add_boolean_value(builder, cfg->cat_general);
    json_builder_set_member_name(builder, "cat_anime"),
    json_builder_add_boolean_value(builder, cfg->cat_anime);
    json_builder_set_member_name(builder, "cat_people"),
    json_builder_add_boolean_value(builder, cfg->cat_people);

    json_builder_set_member_name(builder, "pur_sfw"),
    json_builder_add_boolean_value(builder, cfg->pur_sfw);
    json_builder_set_member_name(builder, "pur_sketchy"),
    json_builder_add_boolean_value(builder, cfg->pur_sketchy);
    json_builder_set_member_name(builder, "pur_nsfw"),
    json_builder_add_boolean_value(builder, cfg->pur_nsfw);

    json_builder_set_member_name(builder, "sort_index"),
    json_builder_add_int_value(builder, cfg->sort_index);
    json_builder_set_member_name(builder, "top_range_index"),
    json_builder_add_int_value(builder, cfg->top_range_index);
    json_builder_set_member_name(builder, "ratio_index"),
    json_builder_add_int_value(builder, cfg->ratio_index);

    if (cfg->min_resolution)
        json_builder_set_member_name(builder, "min_resolution"),
        json_builder_add_string_value(builder, cfg->min_resolution);

    json_builder_set_member_name(builder, "wallpaper_method"),
    json_builder_add_int_value(builder, cfg->wallpaper_method);

    json_builder_end_object(builder);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, json_builder_get_root(builder));
    json_generator_set_pretty(gen, TRUE);
    char *json_str = json_generator_to_data(gen, NULL);

    char *path = CONFIG_PATH_BUILDER();
    char *dir  = g_build_filename(g_get_user_config_dir(), "wh-wall", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(path, json_str, -1, NULL);

    log_info("Config saved to %s", path);

    g_free(json_str);
    g_free(path);
    g_free(dir);
    g_object_unref(gen);
    g_object_unref(builder);
}

void
config_free(AppConfig *cfg)
{
    if (!cfg) return;
    g_free(cfg->api_key);
    g_free(cfg->download_dir);
    g_free(cfg->min_resolution);
    g_free(cfg);
}
