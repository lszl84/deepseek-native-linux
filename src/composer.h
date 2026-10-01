#pragma once
#include <gtk/gtk.h>
#include "settings.h"

typedef struct Composer Composer;

typedef struct {
    void (*send)(const char *text, GPtrArray *images /* ImageData*, taken */, gpointer data);
    void (*stop)(gpointer data);
    void (*permission_changed)(PermissionMode mode, gpointer data);
    void (*model_changed)(const char *model, Effort effort, gpointer data);
    void (*slash)(const char *command, gpointer data);
    const char *(*workspace)(gpointer data);
    gpointer data;
} ComposerCallbacks;

Composer *composer_new(ComposerCallbacks cb);
GtkWidget *composer_widget(Composer *c);
void composer_set_running(Composer *c, gboolean running);
void composer_set_placeholder(Composer *c, const char *text);
void composer_set_permission(Composer *c, PermissionMode p);
void composer_set_model(Composer *c, const char *model, Effort effort);
PermissionMode composer_permission(Composer *c);
const char *composer_model(Composer *c);
Effort composer_effort(Composer *c);
void composer_focus(Composer *c);
void composer_set_text(Composer *c, const char *text);
void composer_insert_text(Composer *c, const char *text);
void composer_add_files(Composer *c, GSList *paths);
void composer_forward_key(Composer *c, GdkEventKey *ev);

/* Shared slash command list. */
typedef struct { const char *name; const char *detail; } SlashCommand;
extern const SlashCommand SLASH_COMMANDS[];
extern const int N_SLASH;

void composer_debug_popovers(Composer *c, GtkWidget **out, int *n);
