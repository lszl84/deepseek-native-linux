#pragma once
#include <gtk/gtk.h>
#include "session.h"

typedef struct Chat Chat;

typedef struct {
    void (*started_session)(Session *s, gpointer data);
    void (*request_new)(gpointer data);
    void (*request_workspace)(gpointer data);
    void (*request_settings)(gpointer data);
    void (*request_rename)(gpointer data);
    void (*meta_changed)(gpointer data);
    gpointer data;
} ChatCallbacks;

Chat *chat_new(ChatCallbacks cb);
GtkWidget *chat_widget(Chat *c);
GtkWidget *chat_transcript(Chat *c);
void chat_show(Chat *c, Session *s);
Session *chat_session(Chat *c);
void chat_set_pending_workspace(Chat *c, const char *path);
const char *chat_pending_workspace(Chat *c);
const char *chat_workspace(Chat *c);
void chat_stop(Chat *c);
void chat_retry(Chat *c);
void chat_focus(Chat *c);
char *chat_last_response(Chat *c);
void chat_export(Chat *c);
void chat_theme_changed(Chat *c);
void chat_expand_all(Chat *c);
void chat_compose(Chat *c, const char *text);
void chat_send_text(Chat *c, const char *text);
void chat_copy_last(Chat *c);

#ifdef DSN_TEST_HOOKS
void chat_debug_popovers(Chat *c, GtkWidget **out, int *n);
#endif
