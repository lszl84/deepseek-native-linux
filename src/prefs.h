#pragma once
#include <gtk/gtk.h>

/* Shows the settings window. `on_change` runs after any setting changes. */
void prefs_show(GtkWindow *parent, void (*on_change)(gpointer), gpointer data);
GtkWidget *prefs_window(void);
