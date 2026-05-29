#include "window.h"
#include "api.h"
#include "config.h"
#include "log.h"
#include <gtk/gtk.h>
#include <curl/curl.h>

/* custom log writer that prepends timestamps and ensures stderr */
static GLogWriterOutput
log_writer(GLogLevelFlags flags, const GLogField *fields, gsize n_fields,
           gpointer user_data)
{
    (void)user_data;

    /* build timestamp */
    GDateTime *now = g_date_time_new_now_local();
    char *ts = g_date_time_format(now, "%H:%M:%S");
    g_date_time_unref(now);

    /* find message and domain fields */
    const char *msg = NULL;
    const char *domain = NULL;
    for (gsize i = 0; i < n_fields; i++) {
        if (g_strcmp0(fields[i].key, "MESSAGE") == 0)
            msg = (const char *)fields[i].value;
        else if (g_strcmp0(fields[i].key, "GLIB_DOMAIN") == 0)
            domain = (const char *)fields[i].value;
    }

    const char *level_str = "UNKNOWN";
    if (flags & G_LOG_LEVEL_ERROR)           level_str = "ERROR";
    else if (flags & G_LOG_LEVEL_CRITICAL)   level_str = "CRITICAL";
    else if (flags & G_LOG_LEVEL_WARNING)    level_str = "WARNING";
    else if (flags & G_LOG_LEVEL_MESSAGE)    level_str = "MESSAGE";
    else if (flags & G_LOG_LEVEL_INFO)       level_str = "INFO";
    else if (flags & G_LOG_LEVEL_DEBUG)      level_str = "DEBUG";

    fprintf(stderr, "%s [%s] %s: %s\n",
            ts, level_str, domain ? domain : "-", msg ? msg : "");
    g_free(ts);

    return G_LOG_WRITER_HANDLED;
}

static void
on_activate(GtkApplication *app, gpointer user_data)
{
    AppConfig *cfg = user_data;
    log_info("Application activated");
    GtkWidget *win = wh_window_new(app, cfg);
    gtk_window_present(GTK_WINDOW(win));
}

int
main(int argc, char **argv)
{
    /* install custom log writer */
    g_log_set_writer_func(log_writer, NULL, NULL);

    /* show debug and info messages */
    g_setenv("G_MESSAGES_DEBUG", "wh-wall", FALSE);

    log_info("wh-wall %s starting", "0.1.0");

    /* load config */
    AppConfig *cfg = config_load();
    api_set_config(cfg);

    /* enable libcurl verbose mode if environment variable is set */
    if (g_getenv("WH_WALL_CURL_VERBOSE"))
        curl_global_init(CURL_GLOBAL_ALL);
    else
        curl_global_init(CURL_GLOBAL_DEFAULT);

    GtkApplication *app = gtk_application_new("cc.wallhaven.wh-wall",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), cfg);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    curl_global_cleanup();
    config_free(cfg);
    log_info("wh-wall exiting (status=%d)", status);
    return status;
}
