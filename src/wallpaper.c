#include "wallpaper.h"
#include "log.h"
#include <curl/curl.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* download                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    FILE          *fp;
    GtkProgressBar *progress;
    curl_off_t    total;
    curl_off_t    now;
} DownloadCtx;

static size_t
download_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    DownloadCtx *ctx = userdata;
    size_t written = fwrite(ptr, size, nmemb, ctx->fp);
    ctx->now += written;
    return written;
}

static int
download_progress_cb(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                     curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal; (void)ulnow;
    DownloadCtx *ctx = userdata;
    ctx->total = dltotal;
    ctx->now   = dlnow;
    return 0;
}

static gboolean
update_progress_bar(gpointer user_data)
{
    DownloadCtx *ctx = user_data;
    if (!ctx->progress || !GTK_IS_PROGRESS_BAR(ctx->progress))
        return G_SOURCE_REMOVE;
    if (ctx->total > 0) {
        double frac = (double)ctx->now / (double)ctx->total;
        if (frac > 1.0) frac = 1.0;
        gtk_progress_bar_set_fraction(ctx->progress, frac);
    }
    return G_SOURCE_CONTINUE;
}

char *
wallpaper_get_path(WallpaperInfo *info, const char *dest_dir)
{
    if (!info || !info->id || !dest_dir) return NULL;

    /* extract extension from url, or use the path part */
    const char *ext = ".jpg";
    if (info->url) {
        const char *dot = strrchr(info->url, '.');
        if (dot) {
            /* check that the extension looks like a file extension (short, alphanumeric) */
            const char *slash = strrchr(info->url, '/');
            if (slash && dot > slash) {
                /* only take extension if dot is after last slash */
                const char *p = dot;
                bool valid = TRUE;
                while (*++p) {
                    if (!g_ascii_isalnum(*p)) { valid = FALSE; break; }
                }
                if (valid && (p - dot) <= 6)
                    ext = dot;
            }
        }
    }
    return g_strdup_printf("%s/%s%s", dest_dir, info->id, ext);
}

bool
wallpaper_download(WallpaperInfo *info, const char *dest_dir,
                    GtkProgressBar *progress)
{
    if (!info || !info->url || !dest_dir) return FALSE;

    char *filename = wallpaper_get_path(info, dest_dir);
    char *key      = api_get_api_key();

    /* construct URL with api key for full resolution */
    const char *dl_url = info->url;
    char *api_url = NULL;
    if (key && key[0]) {
        api_url = g_strdup_printf("%s?apikey=%s", info->url, key);
        dl_url = api_url;
    }

    CURL        *curl = curl_easy_init();
    DownloadCtx  ctx  = {NULL, progress, 0, 0};
    CURLcode     res  = CURLE_FAILED_INIT;
    guint        timer_id = 0;

    if (progress)
        timer_id = g_timeout_add(100, update_progress_bar, &ctx);

    if (curl) {
        ctx.fp = fopen(filename, "wb");
        if (ctx.fp) {
            /* set headers to mimic a real browser */
            struct curl_slist *headers = NULL;
            headers = curl_slist_append(headers, "Referer: https://wallhaven.cc/");
            headers = curl_slist_append(headers, "Origin: https://wallhaven.cc");

            log_info("Downloading: %s", dl_url);
            curl_easy_setopt(curl, CURLOPT_URL, dl_url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, download_progress_cb);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT,
                "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 wh-wall/0.1");
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            res = curl_easy_perform(curl);

            /* check HTTP status code */
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code != 200)
                log_error("HTTP %ld for %s", http_code, dl_url);

            curl_slist_free_all(headers);
            fclose(ctx.fp);
        } else {
            log_error("Cannot open file for writing: %s", filename);
        }
        curl_easy_cleanup(curl);
    } else {
        log_error("curl_easy_init() failed for download");
    }

    if (timer_id)
        g_source_remove(timer_id);

    /* set progress to 100% on completion */
    if (progress && GTK_IS_PROGRESS_BAR(progress))
        gtk_progress_bar_set_fraction(progress, 1.0);

    g_free(api_url);
    g_free(key);

    /* verify the file was written */
    bool ok = FALSE;
    if (res == CURLE_OK) {
        struct stat st;
        if (stat(filename, &st) == 0 && st.st_size > 4096) {
            /* also check the first bytes are an image, not HTML */
            FILE *check = fopen(filename, "rb");
            if (check) {
                unsigned char hdr[4];
                size_t n = fread(hdr, 1, 4, check);
                fclose(check);
                if (n >= 2 && ((hdr[0] == 0xFF && hdr[1] == 0xD8) ||      /* JPEG */
                               (n >= 4 && hdr[0] == 0x89 && hdr[1] == 'P' &&
                                hdr[2] == 'N' && hdr[3] == 'G') ||       /* PNG */
                               (n >= 4 && hdr[0] == 'R' && hdr[1] == 'I' &&
                                hdr[2] == 'F' && hdr[3] == 'F'))) {      /* WebP */
                    log_info("Downloaded %ld bytes to %s", (long)st.st_size, filename);
                    ok = TRUE;
                } else {
                    log_error("Downloaded file is not an image (%02x%02x%02x%02x): %s",
                               n > 0 ? hdr[0] : 0, n > 1 ? hdr[1] : 0,
                               n > 2 ? hdr[2] : 0, n > 3 ? hdr[3] : 0, filename);
                }
            }
        } else if (stat(filename, &st) == 0) {
            log_error("File too small (%ld bytes), download likely failed: %s",
                       (long)st.st_size, filename);
        } else {
            log_error("stat() failed after download: %s", filename);
        }
    } else {
        log_error("curl download failed: %s", curl_easy_strerror(res));
    }

    if (!ok) {
        log_warn("Removing incomplete download: %s", filename);
        unlink(filename);
    }

    g_free(filename);
    return ok;
}

/* ------------------------------------------------------------------ */
/* set as desktop background                                           */
/* method: 0=auto, 1=GNOME, 2=KDE, 3=hyprpaper                       */
/* ------------------------------------------------------------------ */
void
wallpaper_set_as_background(const char *path, int method)
{
    if (!path) return;

    log_info("Setting wallpaper: %s (method=%d)", path, method);

    if (method == 3) {
        /* hyprpaper */
        char *cmd = g_strdup_printf(
            "hyprctl hyprpaper unload all 2>/dev/null; "
            "hyprctl hyprpaper preload \"%s\"; "
            "hyprctl hyprpaper wallpaper \",%s\"",
            path, path);
        int ret = system(cmd);
        if (ret != 0)
            log_error("hyprpaper command failed (ret=%d)", ret);
        g_free(cmd);
        return;
    }

    if (method == 1) {
        /* GNOME only */
        char *uri = g_filename_to_uri(path, NULL, NULL);
        char *cmd = g_strdup_printf(
            "gsettings set org.gnome.desktop.background picture-uri-dark \"%s\" && "
            "gsettings set org.gnome.desktop.background picture-uri \"%s\"",
            uri, uri);
        int ret = system(cmd);
        if (ret != 0)
            log_error("GNOME gsettings failed (ret=%d)", ret);
        g_free(cmd);
        g_free(uri);
        return;
    }

    if (method == 2) {
        /* KDE only */
        char *cmd = g_strdup_printf(
            "dbus-send --session --dest=org.kde.plasmashell --type=method_call "
            "/PlasmaShell org.kde.PlasmaShell.evaluateScript "
            "\"string:var allDesktops = desktops();"
            "for (i=0;i<allDesktops.length;i++) {"
            "  d = allDesktops[i];"
            "  d.wallpaperPlugin = 'org.kde.image';"
            "  d.currentConfigGroup = ['Wallpaper', 'org.kde.image', 'General'];"
            "  d.writeConfig('Image', 'file://%s');"
            "}\"'",
            path);
        int ret = system(cmd);
        if (ret != 0)
            log_error("KDE Plasma command failed (ret=%d)", ret);
        g_free(cmd);
        return;
    }

    /* method == 0: auto-detect */
    /* Try GNOME first, then KDE fallback */
    char *uri = g_filename_to_uri(path, NULL, NULL);

    char *cmd = g_strdup_printf(
        "gsettings set org.gnome.desktop.background picture-uri-dark \"%s\" && "
        "gsettings set org.gnome.desktop.background picture-uri \"%s\"",
        uri, uri);

    int ret = system(cmd);
    if (ret != 0) {
        log_warn("GNOME gsettings failed (ret=%d), trying KDE fallback", ret);
        g_free(cmd);
        cmd = g_strdup_printf(
            "dbus-send --session --dest=org.kde.plasmashell --type=method_call "
            "/PlasmaShell org.kde.PlasmaShell.evaluateScript "
            "\"string:var allDesktops = desktops();"
            "for (i=0;i<allDesktops.length;i++) {"
            "  d = allDesktops[i];"
            "  d.wallpaperPlugin = 'org.kde.image';"
            "  d.currentConfigGroup = ['Wallpaper', 'org.kde.image', 'General'];"
            "  d.writeConfig('Image', 'file://%s');"
            "}\"'",
            path);
        ret = system(cmd);
        if (ret != 0)
            log_error("KDE Plasma wallpaper setting also failed (ret=%d)", ret);
    }

    g_free(cmd);
    g_free(uri);
}

/* ------------------------------------------------------------------ */
/* open in browser                                                     */
/* ------------------------------------------------------------------ */
void
wallpaper_open_in_browser(const char *id)
{
    if (!id) return;
    char *url = g_strdup_printf("https://wallhaven.cc/w/%s", id);
    char *cmd = g_strdup_printf("xdg-open \"%s\" 2>/dev/null", url);
    log_info("Opening browser: %s", url);
    system(cmd);
    g_free(cmd);
    g_free(url);
}
