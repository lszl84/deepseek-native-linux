#pragma once
#include "util.h"

typedef enum { EFFORT_OFF, EFFORT_LOW, EFFORT_HIGH, EFFORT_MAX } Effort;
typedef enum { PERM_READ_ONLY, PERM_WORKSPACE_WRITE, PERM_FULL_ACCESS } PermissionMode;
typedef enum { APPEAR_SYSTEM, APPEAR_LIGHT, APPEAR_DARK } Appearance;

const char *effort_raw(Effort e);
const char *effort_label(Effort e);
Effort effort_parse(const char *s);
const char *perm_raw(PermissionMode p);
const char *perm_label(PermissionMode p);
const char *perm_detail(PermissionMode p);
const char *perm_icon(PermissionMode p);
PermissionMode perm_parse(const char *s);

typedef struct {
    const char *id;
    const char *name;
    const char *detail;
} ModelInfo;
extern const ModelInfo MODELS[];
extern const int N_MODELS;
const char *model_name(const char *id);

typedef struct {
    char *base_url;
    char *model;
    Effort effort;
    int max_tokens;
    gint64 context_window;
    Appearance appearance;
    PermissionMode permission;
    int bash_timeout_ms;
    gboolean web_search;
    gboolean subagents;
    gboolean send_on_enter;
    char *custom_instructions;
    /* window state */
    int win_w, win_h;
    int sidebar_w;
    gboolean sidebar_visible;
} Settings;

Settings *settings(void);
void settings_load(void);
void settings_save(void);
/* Thread-safe copies of string settings. */
char *settings_dup_model(void);
/* Thread-safe setters for string settings (workers read them under the same lock). */
void settings_set_model(const char *v);
void settings_set_base_url(const char *v);
void settings_set_custom(const char *v);
char *settings_dup_custom(void);
char *settings_messages_endpoint(void);

/* API key: resolved lazily from (1) the app's own keyring entry, (2) DEEPSEEK_API_KEY,
 * (3) ~/.dsh/.credentials.yaml. Never written to disk. */
char *api_key_get(void);               /* newly allocated or NULL; may block on the keyring */
gboolean api_key_present(void);         /* cached result; resolves if needed */
const char *api_key_source(void);       /* human description of where the key came from */
gboolean api_key_store(const char *key); /* saves into the app's own keyring entry ("" clears) */
void api_key_resolve_async(void (*done)(gpointer), gpointer data);
