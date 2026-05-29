#ifndef LOG_H
#define LOG_H

#include <glib.h>
#include <stdio.h>
#include <time.h>

#define LOG_DOMAIN "wh-wall"

#define log_info(fmt, ...)  g_log(LOG_DOMAIN, G_LOG_LEVEL_INFO,    fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...)  g_log(LOG_DOMAIN, G_LOG_LEVEL_WARNING, fmt, ##__VA_ARGS__)
#define log_error(fmt, ...) g_log(LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, fmt, ##__VA_ARGS__)

#endif
