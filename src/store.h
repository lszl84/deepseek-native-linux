#pragma once
#include "session.h"

typedef struct {
    char *id;
    char *path;
    gboolean collapsed;
} Workspace;

typedef struct {
    char *id;
    char *cwd;
    char *title;
    double created_at, updated_at;
    gboolean archived;
    gboolean pinned;
} IndexEntry;

typedef enum { STORE_CHANGED, STORE_SESSION_META, STORE_TURN_FINISHED, STORE_ATTENTION } StoreEvent;
typedef void (*StoreObserver)(StoreEvent ev, Session *s, gpointer data);

void store_init(void);
GPtrArray *store_workspaces(void);   /* Workspace* (borrowed) */
GPtrArray *store_index(void);        /* IndexEntry* (borrowed) */
const char *store_last_workspace(void);
void store_set_last_workspace(const char *id);

Workspace *store_add_workspace(const char *path);
void store_remove_workspace(const char *id);
void store_set_collapsed(const char *id, gboolean collapsed);
Workspace *store_workspace(const char *id);
Workspace *store_workspace_for_path(const char *path);
/* Sessions of a workspace, pinned first then most recent. Free with g_ptr_array_unref (borrowed entries). */
GPtrArray *store_sessions_in(const Workspace *w, gboolean include_archived);

Session *store_new_session(const char *cwd);
void store_register(Session *s);
Session *store_session(const char *id);       /* loads lazily from the index */
Session *store_live(const char *id);          /* only if already in memory */
IndexEntry *store_entry(const char *id);
GPtrArray *store_running_sessions(void);       /* Session* borrowed; free container */
void store_trim(const char *keep);
void store_rename(const char *id, const char *title);
void store_set_archived(const char *id, gboolean v);
void store_set_pinned(const char *id, gboolean v);
void store_delete(const char *id);

void store_observe(StoreObserver fn, gpointer data);
void store_emit(StoreEvent ev, Session *s);
/* Called by sessions on every change. */
void store_session_changed(Session *s, int kind);
void store_flush(void);
void store_add_imported(const char *id, const char *cwd, const char *title, double created_at, double updated_at);
