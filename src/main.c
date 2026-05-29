#include "window.h"
#include "api.h"
#include "config.h"
#include "log.h"
#include <gtk/gtk.h>
#include <curl/curl.h>

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
