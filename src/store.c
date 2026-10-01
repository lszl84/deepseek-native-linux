#include "store.h"
#include "agent.h"
#include <string.h>

static GPtrArray *workspaces; /* Workspace* */
static GPtrArray *index_;     /* IndexEntry* */
static char *last_ws;
static GHashTable *live;      /* id -> Session* */
static GPtrArray *observers;
static guint save_source;
static guint reap_source;

typedef struct { StoreObserver fn; gpointer data; } SObs;

static void ws_free(gpointer p) {
    Workspace *w = p;
    g_free(w->id);
    g_free(w->path);
    g_free(w);
}

static void entry_free(gpointer p) {
    IndexEntry *e = p;
    g_free(e->id);
    g_free(e->cwd);
    g_free(e->title);
    g_free(e);
}

static char *store_path(void) { return g_build_filename(app_data_dir(), "workspaces.json", NULL); }

static char *session_file(const char *id) {
    char *fn = g_strconcat(id, ".jsonl", NULL);
    char *p = g_build_filename(sessions_dir(), fn, NULL);
    g_free(fn);
    return p;
}

void store_init(void) {
    if (workspaces) return;
    workspaces = g_ptr_array_new_with_free_func(ws_free);
    index_ = g_ptr_array_new_with_free_func(entry_free);
    live = g_hash_table_new(g_str_hash, g_str_equal);
    observers = g_ptr_array_new_with_free_func(g_free);
    char *path = store_path();
    gsize len;
    char *data = read_file(path, &len);
    g_free(path);
    if (!data) return;
    JsonNode *n = json_parse_str(data, len);
    g_free(data);
    if (n && JSON_NODE_HOLDS_OBJECT(n)) {
        JsonObject *o = json_node_get_object(n);
        JsonArray *ws = jarr(o, "workspaces");
        for (guint i = 0; ws && i < json_array_get_length(ws); i++) {
            JsonObject *w = json_array_get_object_element(ws, i);
            if (!jstr(w, "path")) continue;
            Workspace *x = g_new0(Workspace, 1);
            x->id = g_strdup(jstr(w, "id") ? jstr(w, "id") : "");
            x->path = g_strdup(jstr(w, "path"));
            x->collapsed = jbool(w, "collapsed", FALSE);
            g_ptr_array_add(workspaces, x);
        }
        JsonArray *ss = jarr(o, "sessions");
        for (guint i = 0; ss && i < json_array_get_length(ss); i++) {
            JsonObject *e = json_array_get_object_element(ss, i);
            if (!jstr(e, "id")) continue;
            char *f = session_file(jstr(e, "id"));
            gboolean exists = path_exists(f);
            g_free(f);
            if (!exists) continue;
            IndexEntry *x = g_new0(IndexEntry, 1);
            x->id = g_strdup(jstr(e, "id"));
            x->cwd = g_strdup(jstr(e, "cwd") ? jstr(e, "cwd") : "");
            x->title = g_strdup(jstr(e, "title") ? jstr(e, "title") : "Session");
            x->created_at = jdbl(e, "createdAt", 0);
            x->updated_at = jdbl(e, "updatedAt", 0);
            x->archived = jbool(e, "archived", FALSE);
            x->pinned = jbool(e, "pinned", FALSE);
            g_ptr_array_add(index_, x);
        }
        if (jstr(o, "lastWorkspaceId")) last_ws = g_strdup(jstr(o, "lastWorkspaceId"));
    }
    if (n) json_node_unref(n);
}

void store_flush(void) {
    if (save_source) { g_source_remove(save_source); save_source = 0; }
    JsonObject *o = jo_new();
    JsonArray *ws = json_array_new();
    for (guint i = 0; i < workspaces->len; i++) {
        Workspace *w = g_ptr_array_index(workspaces, i);
        JsonObject *x = jo_new();
        jo_str(x, "id", w->id);
        jo_str(x, "path", w->path);
        if (w->collapsed) jo_bool(x, "collapsed", TRUE);
        json_array_add_object_element(ws, x);
    }
    jo_arr(o, "workspaces", ws);
    JsonArray *ss = json_array_new();
    for (guint i = 0; i < index_->len; i++) {
        IndexEntry *e = g_ptr_array_index(index_, i);
        JsonObject *x = jo_new();
        jo_str(x, "id", e->id);
        jo_str(x, "cwd", e->cwd);
        jo_str(x, "title", e->title);
        jo_dbl(x, "createdAt", e->created_at);
        jo_dbl(x, "updatedAt", e->updated_at);
        if (e->archived) jo_bool(x, "archived", TRUE);
        if (e->pinned) jo_bool(x, "pinned", TRUE);
        json_array_add_object_element(ss, x);
    }
    jo_arr(o, "sessions", ss);
    if (last_ws) jo_str(o, "lastWorkspaceId", last_ws);
    JsonNode *n = jnode_obj(o);
    json_object_unref(o);
    char *s = json_to_str(n, TRUE);
    json_node_unref(n);
    char *path = store_path();
    write_file_atomic(path, s, -1, 0600);
    g_free(path);
    g_free(s);
}

static gboolean save_cb(gpointer p) {
    save_source = 0;
    store_flush();
    return G_SOURCE_REMOVE;
}

static void schedule_save(void) {
    if (!save_source) save_source = g_timeout_add(300, save_cb, NULL);
}

void store_observe(StoreObserver fn, gpointer data) {
    SObs *o = g_new(SObs, 1);
    o->fn = fn;
    o->data = data;
    g_ptr_array_add(observers, o);
}

void store_emit(StoreEvent ev, Session *s) {
    for (guint i = 0; i < observers->len; i++) {
        SObs *o = g_ptr_array_index(observers, i);
        o->fn(ev, s, o->data);
    }
}

static void changed(void) {
    schedule_save();
    store_emit(STORE_CHANGED, NULL);
}

GPtrArray *store_workspaces(void) { return workspaces; }
GPtrArray *store_index(void) { return index_; }
const char *store_last_workspace(void) { return last_ws; }

void store_set_last_workspace(const char *id) {
    if (!g_strcmp0(id, last_ws)) return;
    g_free(last_ws);
    last_ws = g_strdup(id);
    schedule_save();
}

Workspace *store_workspace(const char *id) {
    for (guint i = 0; id && i < workspaces->len; i++) {
        Workspace *w = g_ptr_array_index(workspaces, i);
        if (!strcmp(w->id, id)) return w;
    }
    return NULL;
}

Workspace *store_workspace_for_path(const char *path) {
    for (guint i = 0; path && i < workspaces->len; i++) {
        Workspace *w = g_ptr_array_index(workspaces, i);
        if (!strcmp(w->path, path)) return w;
    }
    return NULL;
}

Workspace *store_add_workspace(const char *path) {
    char *p = path_standardize(path);
    Workspace *w = store_workspace_for_path(p);
    if (w) { g_free(p); return w; }
    w = g_new0(Workspace, 1);
    w->id = uuid_new();
    w->path = p;
    g_ptr_array_add(workspaces, w);
    changed();
    return w;
}

void store_remove_workspace(const char *id) {
    for (guint i = 0; i < workspaces->len; i++) {
        Workspace *w = g_ptr_array_index(workspaces, i);
        if (!strcmp(w->id, id)) { g_ptr_array_remove_index(workspaces, i); break; }
    }
    changed();
}

void store_set_collapsed(const char *id, gboolean collapsed) {
    Workspace *w = store_workspace(id);
    if (w && w->collapsed != collapsed) { w->collapsed = collapsed; schedule_save(); }
}

static gint cmp_sessions(gconstpointer a, gconstpointer b) {
    const IndexEntry *x = *(IndexEntry **)a, *y = *(IndexEntry **)b;
    if (x->pinned != y->pinned) return x->pinned ? -1 : 1;
    return x->updated_at > y->updated_at ? -1 : x->updated_at < y->updated_at ? 1 : 0;
}

GPtrArray *store_sessions_in(const Workspace *w, gboolean include_archived) {
    GPtrArray *out = g_ptr_array_new();
    for (guint i = 0; i < index_->len; i++) {
        IndexEntry *e = g_ptr_array_index(index_, i);
        if (!strcmp(e->cwd, w->path) && (include_archived || !e->archived)) g_ptr_array_add(out, e);
    }
    g_ptr_array_sort(out, cmp_sessions);
    return out;
}

IndexEntry *store_entry(const char *id) {
    for (guint i = 0; id && i < index_->len; i++) {
        IndexEntry *e = g_ptr_array_index(index_, i);
        if (!strcmp(e->id, id)) return e;
    }
    return NULL;
}

Session *store_new_session(const char *cwd) {
    Session *s = session_new(cwd);
    g_hash_table_insert(live, s->id, s);
    return s;
}

void store_register(Session *s) {
    IndexEntry *e = store_entry(s->id);
    if (e) {
        g_free(e->title);
        e->title = g_strdup(s->title);
        e->updated_at = s->updated_at;
    } else {
        e = g_new0(IndexEntry, 1);
        e->id = g_strdup(s->id);
        e->cwd = g_strdup(s->cwd);
        e->title = g_strdup(s->title);
        e->created_at = s->created_at;
        e->updated_at = s->updated_at;
        g_ptr_array_add(index_, e);
        if (!store_workspace_for_path(s->cwd)) store_add_workspace(s->cwd);
    }
    changed();
}

void store_session_changed(Session *s, int kind) {
    if (s->ephemeral) return;
    if (kind == CHANGE_META || kind == CHANGE_RESET) {
        IndexEntry *e = store_entry(s->id);
        if (e && (g_strcmp0(e->title, s->title) || e->updated_at != s->updated_at)) {
            g_free(e->title);
            e->title = g_strdup(s->title);
            e->updated_at = s->updated_at;
            schedule_save();
        }
        store_emit(STORE_SESSION_META, s);
    }
}

Session *store_live(const char *id) { return id ? g_hash_table_lookup(live, id) : NULL; }

Session *store_session(const char *id) {
    Session *s = store_live(id);
    if (s) return s;
    IndexEntry *e = store_entry(id);
    if (!e) return NULL;
    s = session_open(e->id, e->cwd, e->title, e->updated_at, e->created_at);
    g_hash_table_insert(live, s->id, s);
    return s;
}

GPtrArray *store_running_sessions(void) {
    GPtrArray *out = g_ptr_array_new();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, live);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        Session *s = v;
        if (s->running) g_ptr_array_add(out, s);
    }
    return out;
}

void store_trim(const char *keep) {
    GHashTableIter it;
    gpointer k, v;
    GPtrArray *drop = g_ptr_array_new();
    g_hash_table_iter_init(&it, live);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        Session *s = v;
        if (keep && !strcmp(s->id, keep)) continue;
        if (s->running || s->observers->len > 0 || !store_entry(s->id)) continue;
        if (s->agent && agent_has_running_jobs(s->agent)) continue;
        g_ptr_array_add(drop, s);
    }
    for (guint i = 0; i < drop->len; i++) {
        Session *s = g_ptr_array_index(drop, i);
        g_hash_table_remove(live, s->id);
        if (s->agent) agent_free(s->agent);
        session_free(s);
    }
    g_ptr_array_free(drop, TRUE);
}

void store_rename(const char *id, const char *title) {
    Session *s = store_session(id);
    if (s) session_set_title(s, title);
    IndexEntry *e = store_entry(id);
    if (e) { g_free(e->title); e->title = g_strdup(title); }
    changed();
}

void store_set_archived(const char *id, gboolean v) {
    IndexEntry *e = store_entry(id);
    if (e) { e->archived = v; changed(); }
}

void store_set_pinned(const char *id, gboolean v) {
    IndexEntry *e = store_entry(id);
    if (e) { e->pinned = v; changed(); }
}

/* Frees deleted sessions once nothing references them any more. */
static gboolean reap_cb(gpointer p) {
    GHashTableIter it;
    gpointer k, v;
    GPtrArray *drop = g_ptr_array_new();
    gboolean pending = FALSE;
    g_hash_table_iter_init(&it, live);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        Session *s = v;
        if (!s->deleted) continue;
        if (s->running || s->observers->len || (s->agent && agent_has_running_jobs(s->agent))) { pending = TRUE; continue; }
        g_ptr_array_add(drop, s);
    }
    for (guint i = 0; i < drop->len; i++) {
        Session *s = g_ptr_array_index(drop, i);
        g_hash_table_remove(live, s->id);
        if (s->agent) agent_free(s->agent);
        session_free(s);
    }
    g_ptr_array_free(drop, TRUE);
    if (!pending) reap_source = 0;
    return pending ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

void store_delete(const char *id) {
    Session *s = store_session(id);
    if (s) {
        if (s->agent) {
            g_ptr_array_set_size(s->queued_inputs, 0);
            g_ptr_array_set_size(s->queued_images, 0);
            g_ptr_array_set_size(s->agent->pending_notices, 0);
            agent_cancel(s->agent);
            jobs_kill_all(s->agent->jobs);
        }
        session_delete_file(s);
    }
    for (guint i = 0; i < index_->len; i++) {
        IndexEntry *e = g_ptr_array_index(index_, i);
        if (!strcmp(e->id, id)) { g_ptr_array_remove_index(index_, i); break; }
    }
    /* The session object stays alive until its agent and jobs stop; the reaper frees it. */
    if (s && !reap_source && reap_cb(NULL)) reap_source = g_timeout_add(500, reap_cb, NULL);
    changed();
}

void store_add_imported(const char *id, const char *cwd, const char *title, double created_at, double updated_at) {
    if (store_entry(id)) return;
    IndexEntry *e = g_new0(IndexEntry, 1);
    e->id = g_strdup(id);
    e->cwd = g_strdup(cwd);
    e->title = g_strdup(title);
    e->created_at = created_at;
    e->updated_at = updated_at;
    g_ptr_array_add(index_, e);
    if (!store_workspace_for_path(cwd)) store_add_workspace(cwd);
    changed();
}
