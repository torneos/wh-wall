#include "api.h"
#include "config.h"
#include "log.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* global config pointer set by main */
static AppConfig *g_config = NULL;

void
api_set_config(AppConfig *cfg)
{
    g_config = cfg;
}

void
wallpaper_info_free(WallpaperInfo *info)
{
    if (!info) return;
    g_free(info->id);
    g_free(info->url);
    g_free(info->thumb_url);
    g_free(info->resolution);
    g_free(info->category);
    g_free(info->purity);
    g_free(info);
}

/* ------------------------------------------------------------------ */
/* libcurl write callback                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    char *data;
    size_t len;
} CurlBuffer;

static size_t
curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    CurlBuffer *buf = userdata;
    size_t      total = size * nmemb;
    char       *tmp = realloc(buf->data, buf->len + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static char *
curl_get(const char *url)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        log_error("curl_easy_init() failed");
        return NULL;
    }

    CurlBuffer buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "wh-wall/0.1");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        log_error("curl request failed: %s (url=%s)", curl_easy_strerror(res), url);
        g_free(buf.data);
        return NULL;
    }
    return buf.data;
}

/* ------------------------------------------------------------------ */
/* API key helpers                                                     */
/* ------------------------------------------------------------------ */

char *
api_get_api_key(void)
{
    if (!g_config || !g_config->api_key || !g_config->api_key[0])
        return NULL;
    return g_strdup(g_config->api_key);
}

bool
api_has_api_key(void)
{
    char *key = api_get_api_key();
    bool  has = (key != NULL && key[0] != '\0');
    g_free(key);
    return has;
}

char *
api_get_username(void)
{
    char *key = api_get_api_key();
    if (!key) return NULL;

    char *user = NULL;
    /* Ask wallhaven API for user settings to get username */
    char url[512];
    snprintf(url, sizeof(url),
             "https://wallhaven.cc/api/v1/settings?apikey=%s", key);

    char *body = curl_get(url);
    if (body) {
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser, body, -1, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root) {
                JsonObject *obj = json_node_get_object(root);
                if (obj) {
                    JsonObject *data = json_object_get_object_member(obj, "data");
                    if (data && json_object_has_member(data, "username")) {
                        user = g_strdup(json_object_get_string_member(data, "username"));
                        log_info("API key verified for user: %s", user);
                    }
                }
            }
        } else {
            log_warn("Failed to parse API settings response");
        }
        g_object_unref(parser);
        g_free(body);
    } else {
        log_warn("Could not fetch API settings (check connection)");
    }

    g_free(key);
    return user;
}

/* ------------------------------------------------------------------ */
/* Search                                                              */
/* ------------------------------------------------------------------ */
JsonArray *
api_search(const char *query, int page, const char *categories,
           const char *purity, const char *sorting, const char *top_range,
           const char *ratios, const char *atleast,
           int *out_last_page)
{
    char *key = api_get_api_key();

    char *encoded_query = curl_easy_escape(NULL, query ? query : "", 0);

    GString *url = g_string_new("https://wallhaven.cc/api/v1/search");
    g_string_append_printf(url, "?q=%s", encoded_query);
    g_string_append_printf(url, "&categories=%s", categories ? categories : "111");
    g_string_append_printf(url, "&purity=%s", purity ? purity : "100");
    g_string_append_printf(url, "&sorting=%s", sorting ? sorting : "date_added");
    if (top_range) g_string_append_printf(url, "&topRange=%s", top_range);
    if (ratios && ratios[0]) g_string_append_printf(url, "&ratios=%s", ratios);
    if (atleast && atleast[0]) g_string_append_printf(url, "&atleast=%s", atleast);
    g_string_append_printf(url, "&page=%d", page);

    if (key && key[0]) {
        g_string_append_printf(url, "&apikey=%s", key);
    }

    curl_free(encoded_query);

    char      *body = curl_get(url->str);
    g_string_free(url, TRUE);
    g_free(key);

    if (!body) {
        log_warn("Search API request failed for query: %s", query ? query : "(empty)");
        return NULL;
    }

    JsonParser *parser = json_parser_new();
    JsonArray  *result = NULL;
    int         last_page = 1;

    if (json_parser_load_from_data(parser, body, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root) {
            JsonObject *obj = json_node_get_object(root);
            if (obj) {
                /* extract meta.last_page */
                if (json_object_has_member(obj, "meta")) {
                    JsonObject *meta = json_object_get_object_member(obj, "meta");
                    if (meta && json_object_has_member(meta, "last_page"))
                        last_page = (int)json_object_get_int_member(meta, "last_page");
                }

                if (json_object_has_member(obj, "data")) {
                    JsonArray *data = json_object_get_array_member(obj, "data");
                if (data) {
                    result = json_array_sized_new(json_array_get_length(data));
                    for (guint i = 0; i < json_array_get_length(data); i++) {
                        JsonObject *item = json_array_get_object_element(data, i);
                        JsonObject *copy = json_object_new();

                        const char *fields[] = {"id", "url", "thumbs", "resolution",
                                                "category", "purity", "favorites", "views",
                                                "path", NULL};
                        for (int f = 0; fields[f]; f++) {
                            if (json_object_has_member(item, fields[f])) {
                                JsonNode *n = json_object_get_member(item, fields[f]);
                                if (JSON_NODE_HOLDS_VALUE(n)) {
                                    json_object_set_member(copy, fields[f],
                                        json_node_copy(n));
                                } else if (JSON_NODE_HOLDS_OBJECT(n) &&
                                           g_strcmp0(fields[f], "thumbs") == 0) {
                                    JsonObject *thumbs = json_node_get_object(n);
                                    if (json_object_has_member(thumbs, "large")) {
                                        json_object_set_string_member(copy, "thumb_url",
                                            json_object_get_string_member(thumbs, "large"));
                                    }
                                } else if (JSON_NODE_HOLDS_OBJECT(n) &&
                                           g_strcmp0(fields[f], "url") == 0) {
                                    /* url field can be object: {"full":"...", "short":"..."} */
                                    JsonObject *url_obj = json_node_get_object(n);
                                    if (json_object_has_member(url_obj, "full")) {
                                        json_object_set_string_member(copy, "url",
                                            json_object_get_string_member(url_obj, "full"));
                                    } else if (json_object_has_member(url_obj, "short")) {
                                        json_object_set_string_member(copy, "url",
                                            json_object_get_string_member(url_obj, "short"));
                                    }
                                }
                            }
                        }

                        /* Use path as direct image URL.
                         * API returns path like "p9/wallhaven-p96lxj.png" or full URL. */
                        const char *path_val = json_object_get_string_member(copy, "path");
                        if (path_val && path_val[0]) {
                            if (g_str_has_prefix(path_val, "https://")) {
                                json_object_set_string_member(copy, "url", path_val);
                            } else {
                                char full_url[512];
                                snprintf(full_url, sizeof(full_url),
                                         "https://w.wallhaven.cc/full/%s", path_val);
                                json_object_set_string_member(copy, "url", full_url);
                            }
                        }
                        json_array_add_object_element(result, copy);
                    }
                }
                }
            }
        }
    }

    g_object_unref(parser);
    g_free(body);
    if (out_last_page) *out_last_page = last_page;
    return result;
}
