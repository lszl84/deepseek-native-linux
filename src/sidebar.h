#pragma once
#include <gtk/gtk.h>

typedef struct Sidebar Sidebar;

typedef struct {
    void (*select_session)(const char *id, gpointer data);
    void (*new_session)(const char *workspace, gpointer data);
    void (*add_workspace)(gpointer data);
    void (*open_settings)(gpointer data);
    void (*rename)(const char *id, gpointer data);
    void (*delete_session)(const char *id, gpointer data);
    gpointer data;
} SidebarCallbacks;

Sidebar *sidebar_new(SidebarCallbacks cb);
GtkWidget *sidebar_widget(Sidebar *s);
void sidebar_select(Sidebar *s, const char *id);
void sidebar_reload(Sidebar *s);
void sidebar_focus_search(Sidebar *s);
void open_in_terminal(const char *path);
