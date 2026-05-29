#include "window.h"
#include "api.h"
#include "wallpaper.h"
#include "config.h"
#include "log.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* application state                                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    AppConfig   *config;
    GtkWidget   *window;
    GtkWidget   *search_entry;
    GtkWidget   *flowbox;
    GtkWidget   *spinner;
    GtkWidget   *status_label;
    GtkWidget   *page_label;
    GtkWidget   *prev_btn;
    GtkWidget   *next_btn;
    GtkWidget   *preview_popover;

    /* category toggles */
    GtkWidget   *cat_general;
    GtkWidget   *cat_anime;
    GtkWidget   *cat_people;

    /* purity toggles */
    GtkWidget   *pur_sfw;
    GtkWidget   *pur_sketchy;
    GtkWidget   *pur_nsfw;

    /* sorting */
    GtkWidget   *sort_combo;

    /* top range */
    GtkWidget   *top_range_combo;

    /* aspect ratio */
    GtkWidget   *ratio_combo;

    /* minimum resolution */
    GtkWidget   *res_entry;

    /* api key */
    GtkWidget   *api_key_entry;

    /* settings */
    GtkWidget   *settings_dialog;
    GtkWidget   *download_dir_entry;
    GtkWidget   *wallpaper_method_combo;
    GtkWidget   *gsk_renderer_combo;

    /* current preview wallpaper data */
    WallpaperInfo *preview_wp;

    /* search state */
    char        *current_query;
    int          current_page;
    int          total_pages;
    GCancellable *search_cancellable;
} WhApp;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
static const char *
get_purity_string(WhApp *app)
{
    static char pur[4];
    pur[0] = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->pur_sfw))     ? '1' : '0';
    pur[1] = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->pur_sketchy)) ? '1' : '0';
    pur[2] = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->pur_nsfw))    ? '1' : '0';
    pur[3] = '\0';
    return pur;
}

static const char *
get_category_string(WhApp *app)
{
    static char cat[4];
    cat[0] = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->cat_general)) ? '1' : '0';
    cat[1] = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->cat_anime))   ? '1' : '0';
    cat[2] = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->cat_people))  ? '1' : '0';
    cat[3] = '\0';
    return cat;
}

static const char *
get_sorting_string(WhApp *app)
{
    int sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->sort_combo));
    const char *vals[] = {"date_added", "relevance", "random", "views", "favorites", "toplist"};
    if (sel < 0 || sel >= 6) return "date_added";
    return vals[sel];
}

static const char *
get_top_range_string(WhApp *app)
{
    const char *sort = get_sorting_string(app);
    if (g_strcmp0(sort, "toplist") != 0) return NULL;

    int sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->top_range_combo));
    const char *vals[] = {"1d", "3d", "1w", "1M", "3M", "6M", "1y"};
    if (sel < 0 || sel >= 7) return "1w";
    return vals[sel];
}

static const char *
get_ratios_string(WhApp *app)
{
    int sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->ratio_combo));
    if (sel <= 0) return NULL;  /* 0 = "Any" */
    const char *vals[] = {
        NULL, "16x9", "16x10", "21x9", "32x9", "48x9",
        "9x16", "10x16", "9x18", "1x1", "3x2", "4x3", "5x4"
    };
    if (sel < 0 || sel >= 13) return NULL;
    return vals[sel];
}

static int
get_ratio_index(WhApp *app)
{
    return gtk_drop_down_get_selected(GTK_DROP_DOWN(app->ratio_combo));
}

static const char *
get_atleast_string(WhApp *app)
{
    return gtk_editable_get_text(GTK_EDITABLE(app->res_entry));
}

static WallpaperInfo *
json_item_to_info(JsonObject *item)
{
    WallpaperInfo *info = g_new0(WallpaperInfo, 1);
    info->id         = g_strdup(json_object_get_string_member(item, "id"));
    info->url        = g_strdup(json_object_get_string_member(item, "url"));
    info->thumb_url  = g_strdup(json_object_get_string_member(item, "thumb_url"));
    info->resolution = g_strdup(json_object_get_string_member(item, "resolution"));
    info->category   = g_strdup(json_object_get_string_member(item, "category"));
    info->purity     = g_strdup(json_object_get_string_member(item, "purity"));
    info->favorites  = (int)json_object_get_int_member(item, "favorites");
    info->views      = (int)json_object_get_int_member(item, "views");
    return info;
}

static void
ensure_download_dir(WhApp *app)
{
    if (app->config->download_dir) {
        g_mkdir_with_parents(app->config->download_dir, 0755);
        return;
    }

    /* default */
    app->config->download_dir = g_build_filename(g_get_home_dir(), "Pictures", "Wallpapers", NULL);
    log_info("Using default download dir: %s", app->config->download_dir);
    g_mkdir_with_parents(app->config->download_dir, 0755);
}

/* ------------------------------------------------------------------ */
/* thumbnail loading (async)                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    WhApp        *app;
    GtkPicture   *picture;
    char         *url;
} ThumbLoadData;

static void
thumb_load_data_free(ThumbLoadData *tld)
{
    g_object_unref(tld->picture);
    g_free(tld->url);
    g_free(tld);
}

typedef struct {
    GtkPicture *picture;
    GBytes     *bytes;
} ThumbResultData;

static gboolean
on_thumb_loaded_idle(gpointer user_data)
{
    ThumbResultData *trd = user_data;
    if (trd->bytes && g_bytes_get_size(trd->bytes) > 0) {
        const unsigned char *data = g_bytes_get_data(trd->bytes, NULL);
        gsize size = g_bytes_get_size(trd->bytes);
        /* JPEG: FF D8, PNG: 89 50 4E 47, WebP: RIFF */
        bool is_image = (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) ||  /* JPEG */
                        (size >= 4 && data[0] == 0x89 && data[1] == 'P' &&
                         data[2] == 'N' && data[3] == 'G') ||                /* PNG */
                        (size >= 4 && data[0] == 'R' && data[1] == 'I' &&
                         data[2] == 'F' && data[3] == 'F');                 /* WebP */
        if (is_image) {
            GdkTexture *texture = gdk_texture_new_from_bytes(trd->bytes, NULL);
            if (texture) {
                gtk_picture_set_paintable(GTK_PICTURE(trd->picture),
                                          GDK_PAINTABLE(texture));
                g_object_unref(texture);
            }
        } else if (size >= 4) {
            log_warn("Thumbnail data not image: %02x %02x %02x %02x (%zu bytes)",
                     data[0], data[1], data[2], data[3], size);
        }
        g_bytes_unref(trd->bytes);
    } else if (trd->bytes) {
        g_bytes_unref(trd->bytes);
    }
    g_object_unref(trd->picture);
    g_free(trd);
    return G_SOURCE_REMOVE;
}

/* Internal callback for libcurl */
static size_t
curl_write_cb_internal(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    typedef struct { char *data; size_t len; } Buf;
    Buf *buf = userdata;
    size_t total = size * nmemb;
    char *tmp = realloc(buf->data, buf->len + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static void
thumb_download_thread(GTask *task, gpointer source, gpointer task_data,
                      GCancellable *cancellable)
{
    ThumbLoadData *tld = task_data;

    /* Download using libcurl */
    CURL *curl = curl_easy_init();
    if (!curl) {
        g_task_return_boolean(task, FALSE);
        return;
    }

    typedef struct { char *data; size_t len; } Buf;
    Buf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, tld->url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb_internal);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "wh-wall/0.1");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || !buf.data || buf.len == 0) {
        if (res != CURLE_OK)
            log_warn("Thumbnail download failed: %s (url=%s)",
                     curl_easy_strerror(res), tld->url);
        else if (buf.data && buf.len == 0)
            log_warn("Thumbnail download empty response (url=%s)", tld->url);
        g_free(buf.data);
        g_task_return_boolean(task, FALSE);
        return;
    }

    GBytes *bytes = g_bytes_new_take(buf.data, buf.len);

    ThumbResultData *trd = g_new0(ThumbResultData, 1);
    trd->picture = tld->picture;
    trd->bytes   = bytes;
    g_object_ref(trd->picture);

    g_task_return_pointer(task, trd, g_free);
}

static void
on_thumb_download_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    ThumbLoadData *tld = user_data;
    ThumbResultData *trd = g_task_propagate_pointer(G_TASK(res), NULL);

    if (trd) {
        g_idle_add(on_thumb_loaded_idle, trd);
    } else {
        g_object_unref(tld->picture);
    }
    thumb_load_data_free(tld);
}

static void
load_thumbnail(WhApp *app, GtkPicture *picture, const char *url)
{
    ThumbLoadData *tld = g_new0(ThumbLoadData, 1);
    tld->app     = app;
    tld->picture = GTK_PICTURE(picture);
    tld->url     = g_strdup(url);
    g_object_ref(picture);

    GCancellable *cancellable = g_cancellable_new();
    GTask *task = g_task_new(NULL, cancellable, on_thumb_download_done, tld);
    g_task_set_task_data(task, tld, NULL);
    g_task_run_in_thread(task, thumb_download_thread);
    g_object_unref(task);
    g_object_unref(cancellable);
}

/* ------------------------------------------------------------------ */
/* preview popover download (async)                                    */
/* ------------------------------------------------------------------ */
static WallpaperInfo *
wallpaper_info_copy(const WallpaperInfo *src)
{
    if (!src) return NULL;
    WallpaperInfo *info = g_new0(WallpaperInfo, 1);
    info->id         = g_strdup(src->id);
    info->url        = g_strdup(src->url);
    info->thumb_url  = g_strdup(src->thumb_url);
    info->resolution = g_strdup(src->resolution);
    info->category   = g_strdup(src->category);
    info->purity     = g_strdup(src->purity);
    info->favorites  = src->favorites;
    info->views      = src->views;
    return info;
}

typedef struct {
    WhApp          *app;
    WallpaperInfo  *info;
    GtkWidget      *progress;
    GtkWidget      *window;
    GtkWidget      *status_label;
    bool           set_bg;
} DownloadTask;

static void
download_task_free(DownloadTask *dt)
{
    wallpaper_info_free(dt->info);
    g_free(dt);
}

static void
download_thread_func(GTask *task, gpointer source, gpointer task_data,
                     GCancellable *cancellable)
{
    DownloadTask *dt = task_data;
    ensure_download_dir(dt->app);
    bool ok = wallpaper_download(dt->info, dt->app->config->download_dir,
                                  GTK_PROGRESS_BAR(dt->progress));
    g_task_return_boolean(task, ok);
}

static void
on_download_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    DownloadTask *dt = user_data;
    gboolean ok = g_task_propagate_boolean(G_TASK(res), NULL);

    if (dt->progress && GTK_IS_PROGRESS_BAR(dt->progress))
        gtk_widget_set_visible(dt->progress, FALSE);

    if (ok) {
        if (dt->set_bg) {
            char *path = wallpaper_get_path(dt->info,
                                             dt->app->config->download_dir);
            wallpaper_set_as_background(path, dt->app->config->wallpaper_method);
            g_free(path);
        }
        char *msg = g_strdup_printf("Downloaded to %s",
                                     dt->app->config->download_dir);
        gtk_label_set_text(GTK_LABEL(dt->status_label), msg);
        g_free(msg);
    } else {
        gtk_label_set_text(GTK_LABEL(dt->status_label), "Download failed.");
    }
}

static void
start_async_download(WhApp *app, bool set_bg)
{
    if (!app->preview_wp || !app->preview_wp->url) return;

    GtkWidget *progress = g_object_get_data(G_OBJECT(app->preview_popover),
                                             "dl-progress");
    if (progress) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), 0.0);
        gtk_widget_set_visible(progress, TRUE);
    }

    DownloadTask *dt = g_new0(DownloadTask, 1);
    dt->app          = app;
    dt->info         = wallpaper_info_copy(app->preview_wp);
    dt->progress     = progress;
    dt->window       = app->preview_popover;
    dt->status_label = app->status_label;
    dt->set_bg       = set_bg;

    GCancellable *cancellable = g_cancellable_new();
    GTask *task = g_task_new(NULL, cancellable, on_download_done, dt);
    g_task_set_task_data(task, dt, (GDestroyNotify)download_task_free);
    g_task_run_in_thread(task, download_thread_func);
    g_object_unref(task);
    g_object_unref(cancellable);
}

static void
on_preview_set_bg(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    WhApp *app = user_data;
    start_async_download(app, TRUE);
}

static void
on_preview_download(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    WhApp *app = user_data;
    start_async_download(app, FALSE);
}

static void
on_preview_copy(GtkButton *btn, gpointer user_data)
{
    WhApp *app = user_data;
    if (!app->preview_wp || !app->preview_wp->thumb_url) return;

    GdkClipboard *clipboard = gdk_display_get_clipboard(
        gdk_display_get_default());

    /* Copy URL to clipboard */
    gdk_clipboard_set_text(clipboard, app->preview_wp->url);

    gtk_label_set_text(GTK_LABEL(app->status_label), "URL copied to clipboard");

    gtk_window_destroy(GTK_WINDOW(app->preview_popover));
}

static void
on_preview_destroy(gpointer data)
{
    *(GtkWidget **)data = NULL;
}

static void
show_preview(WhApp *app, WallpaperInfo *info)
{
    if (app->preview_wp)
        wallpaper_info_free(app->preview_wp);
    app->preview_wp = wallpaper_info_copy(info);

    /* destroy previous preview window if it exists */
    if (app->preview_popover) {
        gtk_window_destroy(GTK_WINDOW(app->preview_popover));
        app->preview_popover = NULL;
    }

    /* create new preview window */
    GtkWidget *pw = gtk_window_new();
    app->preview_popover = pw;
    gtk_window_set_title(GTK_WINDOW(pw), info->id ? info->id : "Preview");
    gtk_window_set_default_size(GTK_WINDOW(pw), 900, 675);
    gtk_window_set_resizable(GTK_WINDOW(pw), FALSE);

    /* overlay: image fills window, controls on top */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_window_set_child(GTK_WINDOW(pw), overlay);

    /* image fills entire window */
    GtkWidget *prev_img = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(prev_img), GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(GTK_PICTURE(prev_img), TRUE);
    gtk_widget_set_vexpand(prev_img, TRUE);
    gtk_widget_set_hexpand(prev_img, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), prev_img);

    /* load thumbnail */
    if (info->thumb_url && info->thumb_url[0]) {
        load_thumbnail(app, GTK_PICTURE(prev_img), info->thumb_url);
    }

    /* close button — top right corner */
    GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_halign(close_btn, GTK_ALIGN_END);
    gtk_widget_set_valign(close_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_top(close_btn, 8);
    gtk_widget_set_margin_end(close_btn, 8);
    gtk_widget_add_css_class(close_btn, "circular");
    gtk_widget_add_css_class(close_btn, "preview-close");
    g_signal_connect_swapped(close_btn, "clicked",
        G_CALLBACK(gtk_window_destroy), pw);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), close_btn);

    /* bottom bar — semi-transparent background with info and buttons */
    GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(bottom_bar, GTK_ALIGN_END);
    gtk_widget_add_css_class(bottom_bar, "preview-bottom-bar");
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), bottom_bar);

    /* resolution + stats */
    GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(bottom_bar), info_box);

    GtkWidget *res_lbl = gtk_label_new(info->resolution ? info->resolution : "");
    gtk_label_set_xalign(GTK_LABEL(res_lbl), 0.0);
    gtk_widget_add_css_class(res_lbl, "preview-res");
    gtk_box_append(GTK_BOX(info_box), res_lbl);

    char *info_text = g_strdup_printf("♥ %d  •  👁 %d", info->favorites, info->views);
    GtkWidget *info_lbl = gtk_label_new(info_text);
    gtk_label_set_xalign(GTK_LABEL(info_lbl), 0.0);
    gtk_widget_add_css_class(info_lbl, "preview-stats");
    gtk_box_append(GTK_BOX(info_box), info_lbl);
    g_free(info_text);

    /* spacer */
    GtkWidget *spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bottom_bar), spacer);

    /* progress bar */
    GtkWidget *dl_progress = gtk_progress_bar_new();
    gtk_widget_set_visible(dl_progress, FALSE);
    gtk_widget_set_size_request(dl_progress, 120, -1);
    gtk_box_append(GTK_BOX(bottom_bar), dl_progress);
    g_object_set_data(G_OBJECT(pw), "dl-progress", dl_progress);

    /* action buttons */
    GtkWidget *copy_btn = gtk_button_new_with_label("Copy URL");
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_preview_copy), app);
    gtk_box_append(GTK_BOX(bottom_bar), copy_btn);

    GtkWidget *dl_btn2 = gtk_button_new_with_label("Download");
    g_signal_connect(dl_btn2, "clicked", G_CALLBACK(on_preview_download), app);
    gtk_box_append(GTK_BOX(bottom_bar), dl_btn2);

    GtkWidget *bg_btn = gtk_button_new_with_label("Set as Background");
    gtk_widget_add_css_class(bg_btn, "suggested-action");
    g_signal_connect(bg_btn, "clicked", G_CALLBACK(on_preview_set_bg), app);
    gtk_box_append(GTK_BOX(bottom_bar), bg_btn);

    /* Clean up when preview window is destroyed */
    g_signal_connect_data(pw, "destroy",
        G_CALLBACK(on_preview_destroy),
        g_memdup2(&app->preview_popover, sizeof(gpointer)),
        (GClosureNotify)g_free, 0);

    gtk_window_present(GTK_WINDOW(pw));
}

static void
on_thumb_button_clicked(GtkButton *btn, gpointer user_data)
{
    WhApp *app = user_data;
    WallpaperInfo *info = g_object_get_data(G_OBJECT(btn), "wallpaper-info");
    if (info) {
        log_info("Preview: %s", info->id);
        show_preview(app, info);
    }
}

/* ------------------------------------------------------------------ */
/* build UI for a single wallpaper result                              */
/* ------------------------------------------------------------------ */
static GtkWidget *
create_thumbnail(WhApp *app, WallpaperInfo *info)
{
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "thumbnail-btn");

    /* Calculate height from width (180) and selected ratio */
    int ratio_idx = get_ratio_index(app);
    int pic_w = 180;
    int pic_h = 113; /* default ~16:10 */
    if (ratio_idx == 1)      pic_h = 101; /* 16:9 */
    else if (ratio_idx == 2)  pic_h = 113; /* 16:10 */
    else if (ratio_idx == 3)  pic_h = 77;  /* 21:9 */
    else if (ratio_idx == 4)  pic_h = 51;  /* 32:9 */
    else if (ratio_idx == 5)  pic_h = 34;  /* 48:9 */
    else if (ratio_idx == 6)  pic_h = 320; /* 9:16 */
    else if (ratio_idx == 7)  pic_h = 288; /* 10:16 */
    else if (ratio_idx == 8)  pic_h = 360; /* 9:18 */
    else if (ratio_idx == 9)  pic_h = 180; /* 1:1 */
    else if (ratio_idx == 10) pic_h = 120; /* 3:2 */
    else if (ratio_idx == 11) pic_h = 135; /* 4:3 */
    else if (ratio_idx == 12) pic_h = 144; /* 5:4 */

    /* overlay: picture + semi-transparent info bar on top */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_button_set_child(GTK_BUTTON(btn), overlay);

    /* picture fills the entire overlay */
    GtkWidget *picture = gtk_picture_new();
    gtk_widget_set_size_request(picture, pic_w, pic_h);
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), FALSE);
    gtk_picture_set_paintable(GTK_PICTURE(picture), NULL);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), picture);

    /* load thumbnail */
    if (info->thumb_url)
        load_thumbnail(app, GTK_PICTURE(picture), info->thumb_url);
    else
        log_info("No thumbnail URL for wallpaper %s", info->id);

    /* info bar overlayed at the bottom */
    GtkWidget *info_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(info_bar, GTK_ALIGN_END);
    gtk_widget_add_css_class(info_bar, "thumb-info-bar");
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), info_bar);

    /* resolution */
    GtkWidget *res_label = gtk_label_new(info->resolution ? info->resolution : "");
    gtk_label_set_xalign(GTK_LABEL(res_label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(res_label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(res_label, "thumb-res");
    gtk_box_append(GTK_BOX(info_bar), res_label);

    /* spacer */
    GtkWidget *spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(info_bar), spacer);

    /* stats */
    char *fav_text = g_strdup_printf("♥ %d", info->favorites);
    GtkWidget *fav_label = gtk_label_new(fav_text);
    gtk_widget_add_css_class(fav_label, "thumb-stat");
    gtk_box_append(GTK_BOX(info_bar), fav_label);
    g_free(fav_text);

    char *view_text = g_strdup_printf("👁 %d", info->views);
    GtkWidget *view_label = gtk_label_new(view_text);
    gtk_widget_add_css_class(view_label, "thumb-stat");
    gtk_box_append(GTK_BOX(info_bar), view_label);
    g_free(view_text);

    /* store info on the button (outer widget) */
    g_object_set_data_full(G_OBJECT(btn), "wallpaper-info", info,
                           (GDestroyNotify)wallpaper_info_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_thumb_button_clicked), app);

    /* purity border: sfw=none, sketchy=yellow, nsfw=red */
    if (info->purity) {
        if (g_strcmp0(info->purity, "sketchy") == 0)
            gtk_widget_add_css_class(btn, "purity-sketchy");
        else if (g_strcmp0(info->purity, "nsfw") == 0)
            gtk_widget_add_css_class(btn, "purity-nsfw");
    }

    return btn;
}

/* ------------------------------------------------------------------ */
/* search result processing                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    WhApp  *app;
    JsonArray *results;
    int    page;
    int    last_page;
} SearchResultData;

typedef struct {
    char *query;
    int   page;
    char *cat;
    char *pur;
    char *sort;
    char *topr;
    char *ratios;
    char *atleast;
} SearchParams;

static void
search_params_free(SearchParams *p)
{
    g_free(p->query);
    g_free(p->cat);
    g_free(p->pur);
    g_free(p->sort);
    g_free(p->topr);
    g_free(p->ratios);
    g_free(p->atleast);
}

static gboolean
on_search_complete_idle(gpointer user_data)
{
    SearchResultData *srd = user_data;
    WhApp *app = srd->app;

    gtk_spinner_stop(GTK_SPINNER(app->spinner));
    gtk_widget_set_visible(app->spinner, FALSE);

    /* clear old results */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(app->flowbox)) != NULL)
        gtk_flow_box_remove(GTK_FLOW_BOX(app->flowbox), child);

    guint n_results = srd->results ? json_array_get_length(srd->results) : 0;

    if (!srd->results || n_results == 0) {
        gtk_label_set_text(GTK_LABEL(app->status_label), "No results found.");
        gtk_widget_set_sensitive(app->prev_btn, FALSE);
        gtk_widget_set_sensitive(app->next_btn, FALSE);
        if (srd->results) json_array_unref(srd->results);
        g_free(srd);
        return G_SOURCE_REMOVE;
    }

    for (guint i = 0; i < n_results; i++) {
        JsonObject *item = json_array_get_object_element(srd->results, i);
        WallpaperInfo *info = json_item_to_info(item);
        GtkWidget *thumb = create_thumbnail(app, info);
        gtk_flow_box_append(GTK_FLOW_BOX(app->flowbox), thumb);
    }

    json_array_unref(srd->results);

    char *status = g_strdup_printf("%u results found", n_results);
    gtk_label_set_text(GTK_LABEL(app->status_label), status);
    g_free(status);

    app->total_pages = srd->last_page;
    char *page_text = g_strdup_printf("Page %d / %d", app->current_page, app->total_pages);
    gtk_label_set_text(GTK_LABEL(app->page_label), page_text);
    g_free(page_text);

    gtk_widget_set_sensitive(app->prev_btn, app->current_page > 1);
    gtk_widget_set_sensitive(app->next_btn, app->current_page < app->total_pages);

    g_free(srd);
    return G_SOURCE_REMOVE;
}

static gboolean
on_search_error_idle(gpointer user_data)
{
    WhApp *app = user_data;
    log_error("Search failed for query: %s", app->current_query ? app->current_query : "(empty)");
    gtk_spinner_stop(GTK_SPINNER(app->spinner));
    gtk_widget_set_visible(app->spinner, FALSE);
    gtk_label_set_text(GTK_LABEL(app->status_label),
                       "Search failed. Check your connection.");
    return G_SOURCE_REMOVE;
}

static void
search_thread(GTask *task, gpointer source, gpointer task_data,
              GCancellable *cancellable)
{
    SearchParams *params = task_data;

    int last_page = 1;
    JsonArray *results = api_search(params->query, params->page,
                                     params->cat, params->pur,
                                     params->sort, params->topr,
                                     params->ratios, params->atleast,
                                     &last_page);

    SearchResultData *srd = g_new0(SearchResultData, 1);
    srd->app       = NULL;  /* will be set in callback */
    srd->results   = results;
    srd->page      = params->page;
    srd->last_page = last_page;

    g_task_return_pointer(task, srd, g_free);
}

static void
on_search_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    WhApp *app = user_data;
    SearchResultData *srd = g_task_propagate_pointer(G_TASK(res), NULL);

    if (srd) {
        srd->app = app;
        g_idle_add(on_search_complete_idle, srd);
    } else {
        g_idle_add(on_search_error_idle, app);
    }
}

/* ------------------------------------------------------------------ */
/* search actions                                                      */
/* ------------------------------------------------------------------ */
static void
perform_search(WhApp *app)
{
    /* cancel any running search */
    if (app->search_cancellable) {
        g_cancellable_cancel(app->search_cancellable);
        g_object_unref(app->search_cancellable);
        app->search_cancellable = NULL;
    }

    /* clear old results immediately for responsiveness */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(app->flowbox)) != NULL)
        gtk_flow_box_remove(GTK_FLOW_BOX(app->flowbox), child);

    gtk_widget_set_visible(app->spinner, TRUE);
    gtk_spinner_start(GTK_SPINNER(app->spinner));
    gtk_label_set_text(GTK_LABEL(app->status_label), "Searching...");
    gtk_widget_set_sensitive(app->prev_btn, FALSE);
    gtk_widget_set_sensitive(app->next_btn, FALSE);

    g_free(app->current_query);
    app->current_query = g_strdup(gtk_editable_get_text(
        GTK_EDITABLE(app->search_entry)));

    log_info("Searching: q='%s' page=%d cat=%s pur=%s sort=%s",
             app->current_query, app->current_page,
             get_category_string(app), get_purity_string(app),
             get_sorting_string(app));

    /* Collect filter values on the main thread before passing to worker */
    SearchParams params;
    params.query   = g_strdup(app->current_query);
    params.page    = app->current_page;
    params.cat     = g_strdup(get_category_string(app));
    params.pur     = g_strdup(get_purity_string(app));
    params.sort    = g_strdup(get_sorting_string(app));
    params.topr    = g_strdup(get_top_range_string(app));
    params.ratios  = g_strdup(get_ratios_string(app));
    params.atleast = g_strdup(get_atleast_string(app));

    app->search_cancellable = g_cancellable_new();

    GTask *task = g_task_new(NULL, app->search_cancellable,
                             on_search_done, app);
    g_task_set_task_data(task, g_memdup2(&params, sizeof(params)),
                         (GDestroyNotify)search_params_free);
    g_task_run_in_thread(task, search_thread);
    g_object_unref(task);
}

static void
on_search_activate(GtkSearchEntry *entry, gpointer user_data)
{
    WhApp *app = user_data;
    app->current_page = 1;
    app->total_pages  = 100;
    perform_search(app);
}

static void
on_search_btn_clicked(GtkButton *btn, gpointer user_data)
{
    WhApp *app = user_data;
    app->current_page = 1;
    app->total_pages  = 100;
    perform_search(app);
}

static void
on_prev_page(GtkButton *btn, gpointer user_data)
{
    WhApp *app = user_data;
    if (app->current_page > 1) {
        app->current_page--;
        perform_search(app);
    }
}

static void
on_next_page(GtkButton *btn, gpointer user_data)
{
    WhApp *app = user_data;
    app->current_page++;
    perform_search(app);
}

static void
save_filters_to_config(WhApp *app)
{
    AppConfig *cfg = app->config;
    cfg->cat_general     = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->cat_general));
    cfg->cat_anime       = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->cat_anime));
    cfg->cat_people      = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->cat_people));
    cfg->pur_sfw         = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->pur_sfw));
    cfg->pur_sketchy     = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->pur_sketchy));
    cfg->pur_nsfw        = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->pur_nsfw));
    cfg->sort_index      = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->sort_combo));
    cfg->top_range_index = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->top_range_combo));
    cfg->ratio_index     = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->ratio_combo));
    g_free(cfg->min_resolution);
    cfg->min_resolution  = g_strdup(gtk_editable_get_text(GTK_EDITABLE(app->res_entry)));
    config_save(cfg);
}

static void
on_sort_changed(GtkDropDown *dd, GParamSpec *pspec, gpointer user_data)
{
    WhApp *app = user_data;
    const char *sort = get_sorting_string(app);
    gtk_widget_set_visible(app->top_range_combo,
                           g_strcmp0(sort, "toplist") == 0);
    save_filters_to_config(app);
    /* re-search if we already have results */
    if (app->current_query && app->current_query[0]) {
        app->current_page = 1;
        perform_search(app);
    }
}

static void
on_top_range_changed(GtkDropDown *dd, GParamSpec *pspec, gpointer user_data)
{
    WhApp *app = user_data;
    save_filters_to_config(app);
    if (app->current_query && app->current_query[0]) {
        app->current_page = 1;
        perform_search(app);
    }
}

static void
on_category_toggled(GtkCheckButton *btn, gpointer user_data)
{
    WhApp *app = user_data;
    save_filters_to_config(app);
    if (app->current_query && app->current_query[0]) {
        app->current_page = 1;
        perform_search(app);
    }
}

static void
on_ratio_changed(GtkDropDown *dd, GParamSpec *pspec, gpointer user_data)
{
    WhApp *app = user_data;
    save_filters_to_config(app);
    if (app->current_query && app->current_query[0]) {
        app->current_page = 1;
        perform_search(app);
    }
}

static void
on_resolution_activate(GtkEntry *entry, gpointer user_data)
{
    WhApp *app = user_data;
    save_filters_to_config(app);
    if (app->current_query && app->current_query[0]) {
        app->current_page = 1;
        perform_search(app);
    }
}

/* ------------------------------------------------------------------ */
/* settings dialog — file chooser callbacks                            */
/* ------------------------------------------------------------------ */
static void
on_browse_folder_selected(GObject *src, GAsyncResult *res, gpointer user_data)
{
    WhApp *app = user_data;
    GError *err = NULL;
    GFile *folder = gtk_file_dialog_select_folder_finish(
        GTK_FILE_DIALOG(src), res, &err);
    if (folder) {
        char *path = g_file_get_path(folder);
        gtk_editable_set_text(GTK_EDITABLE(app->download_dir_entry), path);
        g_free(path);
        g_object_unref(folder);
    }
    if (err) g_error_free(err);
}

static void
on_browse_btn(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    WhApp *app = user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Select Download Directory");
    gtk_file_dialog_select_folder(dlg,
        GTK_WINDOW(app->settings_dialog), NULL,
        on_browse_folder_selected, app);
    g_object_unref(dlg);
}

/* ------------------------------------------------------------------ */
/* settings dialog                                                     */
/* ------------------------------------------------------------------ */
static void
on_settings_save(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    WhApp *app = user_data;

    /* save download dir */
    const char *dir = gtk_editable_get_text(GTK_EDITABLE(app->download_dir_entry));
    if (dir && dir[0]) {
        g_free(app->config->download_dir);
        app->config->download_dir = g_strdup(dir);
        g_mkdir_with_parents(app->config->download_dir, 0755);
    }

    /* save API key */
    const char *key = gtk_editable_get_text(GTK_EDITABLE(app->api_key_entry));
    g_free(app->config->api_key);
    app->config->api_key = g_strdup(key);

    /* save wallpaper method */
    app->config->wallpaper_method = gtk_drop_down_get_selected(
        GTK_DROP_DOWN(app->wallpaper_method_combo));

    /* save GSK renderer */
    app->config->gsk_renderer = gtk_drop_down_get_selected(
        GTK_DROP_DOWN(app->gsk_renderer_combo));

    config_save(app->config);

    /* verify the key */
    char *user = api_get_username();
    char *msg;
    if (user) {
        msg = g_strdup_printf("Settings saved. API key verified for %s", user);
        g_free(user);
    } else if (key && key[0]) {
        msg = g_strdup("Settings saved. Could not verify API key.");
    } else {
        msg = g_strdup("Settings saved.");
    }
    gtk_label_set_text(GTK_LABEL(app->status_label), msg);
    g_free(msg);

    gtk_window_destroy(GTK_WINDOW(app->settings_dialog));
    app->settings_dialog = NULL;
}

static void
on_settings_cancel(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    WhApp *app = user_data;
    gtk_window_destroy(GTK_WINDOW(app->settings_dialog));
    app->settings_dialog = NULL;
}

static void
show_settings_dialog(WhApp *app, GtkWidget *parent)
{
    if (app->settings_dialog) {
        gtk_window_present(GTK_WINDOW(app->settings_dialog));
        return;
    }

    app->settings_dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(app->settings_dialog), "Settings");
    gtk_window_set_modal(GTK_WINDOW(app->settings_dialog), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(app->settings_dialog),
                                  GTK_WINDOW(parent));
    gtk_window_set_default_size(GTK_WINDOW(app->settings_dialog), 520, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(app->settings_dialog), vbox);

    /* ---- header area ---- */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(header, 20);
    gtk_widget_set_margin_end(header, 20);
    gtk_widget_set_margin_top(header, 24);
    gtk_widget_set_margin_bottom(header, 16);
    gtk_box_append(GTK_BOX(vbox), header);

    GtkWidget *icon = gtk_image_new_from_icon_name("emblem-system-symbolic");
    gtk_widget_set_size_request(icon, 48, 48);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
    gtk_box_append(GTK_BOX(header), icon);

    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(header), title_box);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='large' weight='bold'>Application Settings</span>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_append(GTK_BOX(title_box), title);

    GtkWidget *subtitle = gtk_label_new("Configure API access and download location");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
    gtk_widget_set_opacity(subtitle, 0.6);
    gtk_box_append(GTK_BOX(title_box), subtitle);

    /* ---- separator ---- */
    GtkWidget *sep_top = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(vbox), sep_top);

    /* ---- API key section ---- */
    GtkWidget *api_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(api_section, 20);
    gtk_widget_set_margin_end(api_section, 20);
    gtk_widget_set_margin_top(api_section, 16);
    gtk_box_append(GTK_BOX(vbox), api_section);

    GtkWidget *api_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(api_title), "<span weight='bold'>API Key</span>");
    gtk_label_set_xalign(GTK_LABEL(api_title), 0.0);
    gtk_box_append(GTK_BOX(api_section), api_title);

    GtkWidget *api_desc = gtk_label_new(
        "Enter your Wallhaven API key to access NSFW content and enjoy higher rate limits.\n"
        "You can get a key at wallhaven.cc/settings/account");
    gtk_label_set_wrap(GTK_LABEL(api_desc), TRUE);
    gtk_label_set_xalign(GTK_LABEL(api_desc), 0.0);
    gtk_widget_set_opacity(api_desc, 0.6);
    gtk_label_set_max_width_chars(GTK_LABEL(api_desc), 60);
    gtk_box_append(GTK_BOX(api_section), api_desc);

    app->api_key_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->api_key_entry), "API key...");
    gtk_entry_set_visibility(GTK_ENTRY(app->api_key_entry), FALSE);
    gtk_editable_set_text(GTK_EDITABLE(app->api_key_entry),
                           app->config->api_key ? app->config->api_key : "");
    gtk_box_append(GTK_BOX(api_section), app->api_key_entry);

    /* ---- separator ---- */
    GtkWidget *sep_mid = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep_mid, 12);
    gtk_widget_set_margin_bottom(sep_mid, 4);
    gtk_box_append(GTK_BOX(vbox), sep_mid);

    /* ---- download section ---- */
    GtkWidget *dl_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(dl_section, 20);
    gtk_widget_set_margin_end(dl_section, 20);
    gtk_widget_set_margin_top(dl_section, 12);
    gtk_box_append(GTK_BOX(vbox), dl_section);

    GtkWidget *dl_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(dl_title), "<span weight='bold'>Download Location</span>");
    gtk_label_set_xalign(GTK_LABEL(dl_title), 0.0);
    gtk_box_append(GTK_BOX(dl_section), dl_title);

    GtkWidget *dl_desc = gtk_label_new("Wallpapers will be saved to this folder.");
    gtk_label_set_xalign(GTK_LABEL(dl_desc), 0.0);
    gtk_widget_set_opacity(dl_desc, 0.6);
    gtk_box_append(GTK_BOX(dl_section), dl_desc);

    GtkWidget *dl_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(dl_section), dl_row);

    app->download_dir_entry = gtk_entry_new();
    gtk_widget_set_hexpand(app->download_dir_entry, TRUE);
    gtk_editable_set_text(GTK_EDITABLE(app->download_dir_entry),
                           app->config->download_dir ? app->config->download_dir : "");
    gtk_box_append(GTK_BOX(dl_row), app->download_dir_entry);

    GtkWidget *browse_btn = gtk_button_new_with_label("Browse...");
    g_signal_connect(browse_btn, "clicked", G_CALLBACK(on_browse_btn), app);
    gtk_box_append(GTK_BOX(dl_row), browse_btn);

    /* ---- separator ---- */
    GtkWidget *sep_mid2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep_mid2, 12);
    gtk_widget_set_margin_bottom(sep_mid2, 4);
    gtk_box_append(GTK_BOX(vbox), sep_mid2);

    /* ---- wallpaper method section ---- */
    GtkWidget *wm_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(wm_section, 20);
    gtk_widget_set_margin_end(wm_section, 20);
    gtk_widget_set_margin_top(wm_section, 12);
    gtk_box_append(GTK_BOX(vbox), wm_section);

    GtkWidget *wm_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(wm_title), "<span weight='bold'>Wallpaper Setter</span>");
    gtk_label_set_xalign(GTK_LABEL(wm_title), 0.0);
    gtk_box_append(GTK_BOX(wm_section), wm_title);

    GtkWidget *wm_desc = gtk_label_new("Choose how to apply wallpapers to your desktop.");
    gtk_label_set_xalign(GTK_LABEL(wm_desc), 0.0);
    gtk_widget_set_opacity(wm_desc, 0.6);
    gtk_box_append(GTK_BOX(wm_section), wm_desc);

    static const char *wm_options[] = {
        "Auto-detect", "GNOME (gsettings)", "KDE Plasma", "hyprpaper", NULL
    };
    app->wallpaper_method_combo = gtk_drop_down_new_from_strings(wm_options);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app->wallpaper_method_combo),
                                app->config->wallpaper_method);
    gtk_box_append(GTK_BOX(wm_section), app->wallpaper_method_combo);

    /* ---- separator ---- */
    GtkWidget *sep_mid3 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep_mid3, 12);
    gtk_widget_set_margin_bottom(sep_mid3, 4);
    gtk_box_append(GTK_BOX(vbox), sep_mid3);

    /* ---- GSK renderer section ---- */
    GtkWidget *gsk_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(gsk_section, 20);
    gtk_widget_set_margin_end(gsk_section, 20);
    gtk_widget_set_margin_top(gsk_section, 12);
    gtk_box_append(GTK_BOX(vbox), gsk_section);

    GtkWidget *gsk_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(gsk_title), "<span weight='bold'>GSK Renderer</span>");
    gtk_label_set_xalign(GTK_LABEL(gsk_title), 0.0);
    gtk_box_append(GTK_BOX(gsk_section), gsk_title);

    GtkWidget *gsk_desc = gtk_label_new("Cairo is stable. Vulkan may be faster but can crash on some setups.\n"
        "Requires app restart to take effect.");
    gtk_label_set_wrap(GTK_LABEL(gsk_desc), TRUE);
    gtk_label_set_xalign(GTK_LABEL(gsk_desc), 0.0);
    gtk_widget_set_opacity(gsk_desc, 0.6);
    gtk_box_append(GTK_BOX(gsk_section), gsk_desc);

    static const char *gsk_options[] = { "Cairo (stable)", "Vulkan", NULL };
    app->gsk_renderer_combo = gtk_drop_down_new_from_strings(gsk_options);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app->gsk_renderer_combo),
                                app->config->gsk_renderer);
    gtk_box_append(GTK_BOX(gsk_section), app->gsk_renderer_combo);

    /* ---- separator ---- */
    GtkWidget *sep_bot = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep_bot, 16);
    gtk_box_append(GTK_BOX(vbox), sep_bot);

    /* ---- buttons ---- */
    GtkWidget *bbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(bbox, 20);
    gtk_widget_set_margin_end(bbox, 20);
    gtk_widget_set_margin_top(bbox, 12);
    gtk_widget_set_margin_bottom(bbox, 16);
    gtk_widget_set_halign(bbox, GTK_ALIGN_END);
    gtk_widget_set_hexpand(bbox, TRUE);
    gtk_box_append(GTK_BOX(vbox), bbox);

    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_settings_cancel), app);
    gtk_box_append(GTK_BOX(bbox), cancel);

    GtkWidget *save = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(save, "suggested-action");
    g_signal_connect(save, "clicked", G_CALLBACK(on_settings_save), app);
    gtk_box_append(GTK_BOX(bbox), save);

    gtk_window_present(GTK_WINDOW(app->settings_dialog));
}

/* ------------------------------------------------------------------ */
/* toolbar buttons                                                     */
/* ------------------------------------------------------------------ */
static void
on_settings_btn(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    WhApp *app = user_data;
    show_settings_dialog(app, app->window);
}

/* ------------------------------------------------------------------ */
/* window creation                                                     */
/* ------------------------------------------------------------------ */
GtkWidget *
wh_window_new(GtkApplication *gtk_app, AppConfig *cfg)
{
    WhApp *app = g_new0(WhApp, 1);
    app->config = cfg;

    GtkWidget *win = gtk_application_window_new(gtk_app);
    app->window = win;
    gtk_window_set_title(GTK_WINDOW(win), "wh-wall — Wallhaven Browser");
    gtk_window_set_default_size(GTK_WINDOW(win), 1100, 750);

    /* ---- CSS provider for purity borders ---- */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".thumbnail-btn {"
        "  background: transparent;"
        "  border: none;"
        "  border-radius: 10px;"
        "  padding: 0;"
        "  overflow: hidden;"
        "  box-shadow: 0 1px 4px alpha(black, 0.12);"
        "  transition: box-shadow 0.2s;"
        "}"
        ".thumbnail-btn:hover {"
        "  box-shadow: 0 4px 20px alpha(black, 0.25);"
        "}"
        ".purity-sketchy {"
        "  box-shadow: 0 0 0 2px rgba(180,160,40,0.5), 0 1px 4px alpha(black, 0.12);"
        "}"
        ".purity-nsfw {"
        "  box-shadow: 0 0 0 2px rgba(180,50,50,0.5), 0 1px 4px alpha(black, 0.12);"
        "}"
        ".thumb-info-bar {"
        "  padding: 4px 8px;"
        "  background: linear-gradient(to top, alpha(black, 0.65), transparent);"
        "}"
        ".thumb-res { font-size: 0.8em; color: white; }"
        ".thumb-stat { font-size: 0.75em; color: rgba(255,255,255,0.7); }"
        ".preview-close { background: alpha(black, 0.45); color: white; min-width: 36px; min-height: 36px; }"
        ".preview-close:hover { background: alpha(red, 0.7); }"
        ".preview-bottom-bar {"
        "  padding: 10px 16px;"
        "  background: linear-gradient(to top, alpha(black, 0.75), alpha(black, 0.4), transparent);"
        "}"
        ".preview-res { color: white; font-size: 1.1em; font-weight: bold; }"
        ".preview-stats { color: rgba(255,255,255,0.65); font-size: 0.85em; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* main vertical box */
    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(win), main_vbox);

    /* ---- header bar ---- */
    GtkWidget *hdr = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hdr), TRUE);

    /* Settings button */
    GtkWidget *settings_btn = gtk_button_new_from_icon_name("emblem-system-symbolic");
    gtk_widget_set_tooltip_text(settings_btn, "Settings");
    g_signal_connect(settings_btn, "clicked", G_CALLBACK(on_settings_btn), app);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hdr), settings_btn);

    gtk_box_append(GTK_BOX(main_vbox), hdr);

    /* ---- search bar ---- */
    GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(search_box, 12);
    gtk_widget_set_margin_end(search_box, 12);
    gtk_widget_set_margin_top(search_box, 12);
    gtk_widget_set_margin_bottom(search_box, 8);
    gtk_box_append(GTK_BOX(main_vbox), search_box);

    app->search_entry = gtk_search_entry_new();
    gtk_text_set_placeholder_text(GTK_TEXT(app->search_entry),
                                   "Search wallpapers (e.g. nature, space, abstract)...");
    gtk_widget_set_hexpand(app->search_entry, TRUE);
    g_signal_connect(app->search_entry, "search-changed",
                     G_CALLBACK(on_search_activate), app);
    gtk_box_append(GTK_BOX(search_box), app->search_entry);

    GtkWidget *search_btn = gtk_button_new_with_label("Search");
    gtk_widget_add_css_class(search_btn, "suggested-action");
    g_signal_connect(search_btn, "clicked", G_CALLBACK(on_search_btn_clicked), app);
    gtk_box_append(GTK_BOX(search_box), search_btn);

    /* ---- filter bar ---- */
    GtkWidget *filter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(filter_box, 12);
    gtk_widget_set_margin_end(filter_box, 12);
    gtk_widget_set_margin_bottom(filter_box, 8);
    gtk_box_append(GTK_BOX(main_vbox), filter_box);

    /* categories */
    GtkWidget *cat_label = gtk_label_new("Categories:");
    gtk_box_append(GTK_BOX(filter_box), cat_label);

    app->cat_general = gtk_check_button_new_with_label("General");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(app->cat_general), cfg->cat_general);
    g_signal_connect(app->cat_general, "toggled", G_CALLBACK(on_category_toggled), app);
    gtk_box_append(GTK_BOX(filter_box), app->cat_general);

    app->cat_anime = gtk_check_button_new_with_label("Anime");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(app->cat_anime), cfg->cat_anime);
    g_signal_connect(app->cat_anime, "toggled", G_CALLBACK(on_category_toggled), app);
    gtk_box_append(GTK_BOX(filter_box), app->cat_anime);

    app->cat_people = gtk_check_button_new_with_label("People");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(app->cat_people), cfg->cat_people);
    g_signal_connect(app->cat_people, "toggled", G_CALLBACK(on_category_toggled), app);
    gtk_box_append(GTK_BOX(filter_box), app->cat_people);

    /* separator */
    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_append(GTK_BOX(filter_box), sep1);

    /* purity */
    GtkWidget *pur_label = gtk_label_new("Purity:");
    gtk_box_append(GTK_BOX(filter_box), pur_label);

    app->pur_sfw = gtk_check_button_new_with_label("SFW");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(app->pur_sfw), cfg->pur_sfw);
    g_signal_connect(app->pur_sfw, "toggled", G_CALLBACK(on_category_toggled), app);
    gtk_box_append(GTK_BOX(filter_box), app->pur_sfw);

    app->pur_sketchy = gtk_check_button_new_with_label("Sketchy");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(app->pur_sketchy), cfg->pur_sketchy);
    g_signal_connect(app->pur_sketchy, "toggled", G_CALLBACK(on_category_toggled), app);
    gtk_box_append(GTK_BOX(filter_box), app->pur_sketchy);

    app->pur_nsfw = gtk_check_button_new_with_label("NSFW");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(app->pur_nsfw), cfg->pur_nsfw);
    g_signal_connect(app->pur_nsfw, "toggled", G_CALLBACK(on_category_toggled), app);
    gtk_box_append(GTK_BOX(filter_box), app->pur_nsfw);

    /* separator */
    GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_append(GTK_BOX(filter_box), sep2);

    /* sorting */
    GtkWidget *sort_label = gtk_label_new("Sort:");
    gtk_box_append(GTK_BOX(filter_box), sort_label);

    static const char *sort_options[] = {
        "Date Added", "Relevance", "Random", "Views", "Favorites", "Top List", NULL
    };
    app->sort_combo = gtk_drop_down_new_from_strings(sort_options);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app->sort_combo), cfg->sort_index);
    g_signal_connect(app->sort_combo, "notify::selected",
                     G_CALLBACK(on_sort_changed), app);
    gtk_box_append(GTK_BOX(filter_box), app->sort_combo);

    /* top range (hidden unless sorting is toplist) */
    static const char *top_options[] = {
        "1 Day", "3 Days", "1 Week", "1 Month", "3 Months", "6 Months", "1 Year", NULL
    };
    app->top_range_combo = gtk_drop_down_new_from_strings(top_options);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app->top_range_combo), cfg->top_range_index);
    gtk_widget_set_visible(app->top_range_combo, cfg->sort_index == 5);
    g_signal_connect(app->top_range_combo, "notify::selected",
                     G_CALLBACK(on_top_range_changed), app);
    gtk_box_append(GTK_BOX(filter_box), app->top_range_combo);

    /* separator */
    GtkWidget *sep3 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_append(GTK_BOX(filter_box), sep3);

    /* aspect ratio */
    GtkWidget *ratio_label = gtk_label_new("Ratio:");
    gtk_box_append(GTK_BOX(filter_box), ratio_label);

    const char *ratio_options[] = {
        "Any", "16:9", "16:10", "21:9", "32:9", "48:9",
        "9:16", "10:16", "9:18", "1:1", "3:2", "4:3", "5:4", NULL
    };
    app->ratio_combo = gtk_drop_down_new_from_strings(ratio_options);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app->ratio_combo), cfg->ratio_index);
    g_signal_connect(app->ratio_combo, "notify::selected",
                     G_CALLBACK(on_ratio_changed), app);
    gtk_box_append(GTK_BOX(filter_box), app->ratio_combo);

    /* separator */
    GtkWidget *sep4 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_append(GTK_BOX(filter_box), sep4);

    /* minimum resolution */
    GtkWidget *res_label = gtk_label_new("Min res:");
    gtk_box_append(GTK_BOX(filter_box), res_label);

    app->res_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->res_entry), "1920x1080");
    gtk_widget_set_size_request(app->res_entry, 110, -1);
    gtk_entry_set_max_length(GTK_ENTRY(app->res_entry), 12);
    if (cfg->min_resolution)
        gtk_editable_set_text(GTK_EDITABLE(app->res_entry), cfg->min_resolution);
    g_signal_connect(app->res_entry, "activate",
                     G_CALLBACK(on_resolution_activate), app);
    gtk_box_append(GTK_BOX(filter_box), app->res_entry);

    /* ---- scrolled results ---- */
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(main_vbox), scrolled);

    app->flowbox = gtk_flow_box_new();
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(app->flowbox), 2);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(app->flowbox), 10);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(app->flowbox), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(app->flowbox), FALSE);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(app->flowbox), 12);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(app->flowbox), 12);

    /* overlay spinner on flowbox */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), overlay);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), app->flowbox);

    app->spinner = gtk_spinner_new();
    gtk_widget_set_size_request(app->spinner, 48, 48);
    gtk_widget_set_halign(app->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(app->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(app->spinner, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), app->spinner);

    /* ---- bottom bar ---- */
    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(bottom, 12);
    gtk_widget_set_margin_end(bottom, 12);
    gtk_widget_set_margin_top(bottom, 8);
    gtk_widget_set_margin_bottom(bottom, 8);
    gtk_box_append(GTK_BOX(main_vbox), bottom);

    app->prev_btn = gtk_button_new_with_label("← Previous");
    gtk_widget_set_sensitive(app->prev_btn, FALSE);
    g_signal_connect(app->prev_btn, "clicked", G_CALLBACK(on_prev_page), app);
    gtk_box_append(GTK_BOX(bottom), app->prev_btn);

    app->page_label = gtk_label_new("");
    gtk_widget_set_halign(app->page_label, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(app->page_label, TRUE);
    gtk_box_append(GTK_BOX(bottom), app->page_label);

    app->status_label = gtk_label_new("Enter a search term to find wallpapers");
    gtk_widget_set_halign(app->status_label, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(app->status_label, TRUE);
    gtk_box_append(GTK_BOX(bottom), app->status_label);

    app->next_btn = gtk_button_new_with_label("Next →");
    gtk_widget_set_sensitive(app->next_btn, FALSE);
    g_signal_connect(app->next_btn, "clicked", G_CALLBACK(on_next_page), app);
    gtk_box_append(GTK_BOX(bottom), app->next_btn);

    /* ---- preview window (created on demand in show_preview) ---- */
    app->preview_popover = NULL;

    /* init state */
    app->current_page = 1;
    app->total_pages  = 100;

    /* start with some trending wallpapers */
    app->current_query = g_strdup("");
    perform_search(app);

    return win;
}
