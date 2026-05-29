#ifndef WINDOW_H
#define WINDOW_H

#include "config.h"
#include <gtk/gtk.h>

GtkWidget *wh_window_new(GtkApplication *app, AppConfig *cfg);

#endif
