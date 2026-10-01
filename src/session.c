#include "session.h"
#include "store.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TOOL_STATUS[] = { "pending", "running", "awaitingApproval", "done", "error", "denied", "cancelled" };

const char *tool_status_raw(ToolStatus s) { return TOOL_STATUS[s]; }

ToolStatus tool_status_parse(const char *s) {
    for (int i = 0; i < 7; i++)
        if (!g_strcmp0(TOOL_STATUS[i], s)) return i;
    return TOOL_DONE;
}

ToolRun *tool_run_new(ToolStatus st) {
    ToolRun *r = g_new0(ToolRun, 1);
    r->status = st;
    return r;
}

ToolRun *tool_run_copy(const ToolRun *s) {
    ToolRun *r = g_new0(ToolRun, 1);
    *r = *s;
    r->output = g_strdup(s->output);
    r->progress = g_strdup(s->progress);
    r->meta = jnode_copy(s->meta);
    return r;
}

void tool_run_free(gpointer p) {
    ToolRun *r = p;
    if (!r) return;
    g_free(r->output);
    g_free(r->progress);
    if (r->meta) json_node_unref(r->meta);
    g_free(r);
}

static JsonObject *meta_obj(const ToolRun *r) {
    return r && r->meta && JSON_NODE_HOLDS_OBJECT(r->meta) ? json_node_get_object(r->meta) : NULL;
}

gint64 tool_meta_int(const ToolRun *r, const char *k, gint64 def) { return jint(meta_obj(r), k, def); }
gboolean tool_meta_has(const ToolRun *r, const char *k) { return jhas(meta_obj(r), k); }
const char *tool_meta_str(const ToolRun *r, const char *k) { return jstr(meta_obj(r), k); }

void todo_free(gpointer p) {
    TodoItem *t = p;
    if (!t) return;
    g_free(t->content);
    g_free(t->status);
    g_free(t);
}

void question_free(gpointer p) {
    Question *q = p;
    if (!q) return;
    g_free(q->id);
    g_free(q->header);
    g_free(q->question);
    g_ptr_array_unref(q->labels);
    g_ptr_array_unref(q->descs);
    g_free(q);
}

static void turn_free(gpointer p) {
    TurnRecord *t = p;
    g_free(t->error);
    g_free(t);
}

static void approval_free(gpointer p) {
    ApprovalRequest *a = p;
    g_free(a->id);
    g_free(a->call_id);
    g_free(a->title);
    g_free(a->detail);
    waiter_unref(a->waiter);
    g_free(a);
}

static void qrequest_free(gpointer p) {
    QuestionRequest *q = p;
    g_free(q->id);
    g_free(q->call_id);
    g_ptr_array_unref(q->questions);
    waiter_unref(q->waiter);
    g_free(q);
}

typedef struct { SessionObserver fn; gpointer data; } Obs;

static void init_containers(Session *s) {
    s->messages = g_ptr_array_new_with_free_func(message_free);
    s->tool_runs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, tool_run_free);
    s->turns = g_ptr_array_new_with_free_func(turn_free);
    s->todos = g_ptr_array_new_with_free_func(todo_free);
    s->observed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    s->pending_approvals = g_ptr_array_new_with_free_func(approval_free);
    s->pending_questions = g_ptr_array_new_with_free_func(qrequest_free);
    s->queued_inputs = g_ptr_array_new_with_free_func(g_free);
    s->queued_images = g_ptr_array_new_with_free_func(image_data_free);
    s->session_approvals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    s->observers = g_ptr_array_new_with_free_func(g_free);
    s->compaction_index = -1;
}

static Session *session_alloc(const char *id, const char *cwd) {
    Session *s = g_new0(Session, 1);
    s->id = g_strdup(id);
    s->cwd = g_strdup(cwd);
    s->title = g_strdup("New Session");
    Settings *st = settings();
    s->model = settings_dup_model();
    s->effort = st->effort;
    s->permission = st->permission;
    char *fn = g_strconcat(id, ".jsonl", NULL);
    s->file_path = g_build_filename(sessions_dir(), fn, NULL);
    g_free(fn);
    init_containers(s);
    return s;
}

Session *session_new(const char *cwd) {
    char *u = uuid_new();
    char *id = g_strconcat("session-", u, NULL);
    Session *s = session_alloc(id, cwd);
    g_free(u);
    g_free(id);
    s->created_at = s->updated_at = now_ts();
    s->loaded = TRUE;
    return s;
}

Session *session_open(const char *id, const char *cwd, const char *title, double updated_at, double created_at) {
    Session *s = session_alloc(id, cwd);
    g_free(s->title);
    s->title = g_strdup(title);
    s->has_title = TRUE;
    s->updated_at = updated_at;
    s->created_at = created_at;
    return s;
}

static void clear_transcript(Session *s) {
    g_ptr_array_set_size(s->messages, 0);
    g_hash_table_remove_all(s->tool_runs);
    g_ptr_array_set_size(s->turns, 0);
    g_ptr_array_set_size(s->todos, 0);
    g_hash_table_remove_all(s->observed);
    s->compaction_index = -1;
    g_clear_pointer(&s->compaction_summary, g_free);
}

void session_free(Session *s) {
    if (!s) return;
    if (s->fh) fclose(s->fh);
    g_ptr_array_unref(s->messages);
    g_hash_table_unref(s->tool_runs);
    g_ptr_array_unref(s->turns);
    g_ptr_array_unref(s->todos);
    g_hash_table_unref(s->observed);
    g_ptr_array_unref(s->pending_approvals);
    g_ptr_array_unref(s->pending_questions);
    g_ptr_array_unref(s->queued_inputs);
    g_ptr_array_unref(s->queued_images);
    g_hash_table_unref(s->session_approvals);
    g_ptr_array_unref(s->observers);
    draft_free(s->draft);
    g_free(s->draft_id);
    g_free(s->compaction_summary);
    g_free(s->status_line);
    g_free(s->last_error);
    g_free(s->id);
    g_free(s->cwd);
    g_free(s->title);
    g_free(s->model);
    g_free(s->file_path);
    g_free(s);
}

/* ---------------- events ---------------- */

static JsonObject *run_to_json(const ToolRun *r) {
    JsonObject *o = jo_new();
    jo_str(o, "status", tool_status_raw(r->status));
    if (r->started_at > 0) jo_dbl(o, "startedAt", r->started_at);
    if (r->finished_at > 0) jo_dbl(o, "finishedAt", r->finished_at);
    if (r->output) jo_str(o, "output", r->output);
    if (r->meta) json_object_set_member(o, "meta", json_node_copy(r->meta));
    return o;
}

static ToolRun *run_from_json(JsonObject *o) {
    ToolRun *r = tool_run_new(tool_status_parse(jstr(o, "status")));
    r->started_at = jdbl(o, "startedAt", 0);
    r->finished_at = jdbl(o, "finishedAt", 0);
    r->output = g_strdup(jstr(o, "output"));
    if (o && json_object_has_member(o, "meta")) {
        JsonNode *m = json_object_get_member(o, "meta");
        if (m && JSON_NODE_HOLDS_OBJECT(m)) r->meta = json_node_copy(m);
    }
    return r;
}

static const char *end_raw(TurnEnd e) {
    switch (e) {
    case END_COMPLETED: return "completed";
    case END_STOPPED: return "stopped";
    case END_ERROR: return "error";
    default: return NULL;
    }
}

static TurnEnd end_parse(const char *s) {
    if (!g_strcmp0(s, "completed")) return END_COMPLETED;
    if (!g_strcmp0(s, "stopped")) return END_STOPPED;
    if (!g_strcmp0(s, "error")) return END_ERROR;
    return END_NONE;
}

static void persist(Session *s, JsonObject *ev /* taken */) {
    if (s->ephemeral) { json_object_unref(ev); return; }
    if (!s->fh) {
        gboolean isnew = !path_exists(s->file_path);
        s->fh = fopen(s->file_path, "a");
        if (!s->fh) { json_object_unref(ev); return; }
        fchmod(fileno(s->fh), 0600);
        if (isnew) {
            JsonObject *h = jo_new();
            jo_str(h, "t", "header");
            jo_str(h, "id", s->id);
            jo_str(h, "cwd", s->cwd);
            jo_dbl(h, "createdAt", s->created_at);
            char *hs = jobj_to_str(h);
            json_object_unref(h);
            fputs(hs, s->fh);
            fputc('\n', s->fh);
            g_free(hs);
        }
    }
    char *line = jobj_to_str(ev);
    json_object_unref(ev);
    fputs(line, s->fh);
    fputc('\n', s->fh);
    fflush(s->fh);
    g_free(line);
}

static void apply_event(Session *s, JsonObject *o) {
    const char *t = jstr(o, "t");
    if (!t) return;
    if (!strcmp(t, "header")) {
        if (jstr(o, "cwd")) { g_free(s->cwd); s->cwd = g_strdup(jstr(o, "cwd")); }
        s->created_at = jdbl(o, "createdAt", s->created_at);
    } else if (!strcmp(t, "message")) {
        Message *m = message_from_json(jobj(o, "m"));
        if (m) g_ptr_array_add(s->messages, m);
    } else if (!strcmp(t, "toolRun")) {
        if (jstr(o, "id")) g_hash_table_replace(s->tool_runs, g_strdup(jstr(o, "id")), run_from_json(jobj(o, "run")));
    } else if (!strcmp(t, "turnStart")) {
        TurnRecord *tr = g_new0(TurnRecord, 1);
        tr->index = jint(o, "index", s->turns->len + 1);
        tr->started_at = jdbl(o, "startedAt", 0);
        tr->message_index = jint(o, "messageIndex", 0);
        g_ptr_array_add(s->turns, tr);
    } else if (!strcmp(t, "turnEnd")) {
        int idx = jint(o, "index", -1);
        for (int i = s->turns->len - 1; i >= 0; i--) {
            TurnRecord *tr = g_ptr_array_index(s->turns, i);
            if (tr->index == idx) {
                tr->ended_at = jdbl(o, "endedAt", 0);
                tr->end = end_parse(jstr(o, "end"));
                g_free(tr->error);
                tr->error = g_strdup(jstr(o, "error"));
                break;
            }
        }
    } else if (!strcmp(t, "title")) {
        g_free(s->title);
        s->title = g_strdup(jstr(o, "title") ? jstr(o, "title") : "Session");
        s->has_title = TRUE;
    } else if (!strcmp(t, "todos")) {
        g_ptr_array_set_size(s->todos, 0);
        JsonArray *a = jarr(o, "todos");
        for (guint i = 0; a && i < json_array_get_length(a); i++) {
            JsonObject *it = json_array_get_object_element(a, i);
            TodoItem *ti = g_new0(TodoItem, 1);
            ti->content = g_strdup(jstr(it, "content") ? jstr(it, "content") : "");
            ti->status = g_strdup(jstr(it, "status") ? jstr(it, "status") : "pending");
            g_ptr_array_add(s->todos, ti);
        }
    } else if (!strcmp(t, "observed")) {
        if (jstr(o, "path")) g_hash_table_add(s->observed, g_strdup(jstr(o, "path")));
    } else if (!strcmp(t, "compaction")) {
        s->compaction_index = jint(o, "index", -1);
        g_free(s->compaction_summary);
        s->compaction_summary = g_strdup(jstr(o, "summary"));
    } else if (!strcmp(t, "config")) {
        if (jstr(o, "model")) { g_free(s->model); s->model = g_strdup(jstr(o, "model")); }
        if (jstr(o, "effort")) s->effort = effort_parse(jstr(o, "effort"));
        if (jstr(o, "permission")) s->permission = perm_parse(jstr(o, "permission"));
    }
}

void session_load(Session *s) {
    if (s->loaded) return;
    s->loaded = TRUE;
    gsize len = 0;
    char *data = read_file(s->file_path, &len);
    if (!data) return;
    JsonParser *p = json_parser_new();
    char *start = data, *end = data + len;
    while (start < end) {
        char *nl = memchr(start, '\n', end - start);
        if (!nl) nl = end;
        if (nl > start && json_parser_load_from_data(p, start, nl - start, NULL)) {
            JsonNode *root = json_parser_get_root(p);
            if (root && JSON_NODE_HOLDS_OBJECT(root)) apply_event(s, json_node_get_object(root));
        }
        start = nl + 1;
    }
    g_object_unref(p);
    g_free(data);
    /* A turn left open by a crash or quit is marked stopped. */
    if (s->turns->len) {
        TurnRecord *last = g_ptr_array_index(s->turns, s->turns->len - 1);
        if (last->end == END_NONE) { last->end = END_STOPPED; last->ended_at = last->started_at; }
    }
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, s->tool_runs);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        ToolRun *r = v;
        if (r->status == TOOL_RUNNING || r->status == TOOL_PENDING || r->status == TOOL_AWAITING) r->status = TOOL_CANCELLED;
    }
    session_repair_dangling(s, FALSE);
}

void session_unload(Session *s) {
    if (!s->loaded || s->running) return;
    clear_transcript(s);
    s->loaded = FALSE;
    if (s->fh) { fclose(s->fh); s->fh = NULL; }
}

void session_delete_file(Session *s) {
    if (s->fh) { fclose(s->fh); s->fh = NULL; }
    s->ephemeral = TRUE; /* nothing more is written for a deleted session */
    s->deleted = TRUE;
    unlink(s->file_path);
}

/* ---------------- observers ---------------- */

guint session_observe(Session *s, SessionObserver fn, gpointer data) {
    Obs *o = g_new(Obs, 1);
    o->fn = fn;
    o->data = data;
    g_ptr_array_add(s->observers, o);
    return s->observers->len;
}

void session_unobserve(Session *s, gpointer data) {
    for (int i = s->observers->len - 1; i >= 0; i--) {
        Obs *o = g_ptr_array_index(s->observers, i);
        if (o->data == data) g_ptr_array_remove_index(s->observers, i);
    }
}

void session_notify(Session *s, ChangeKind kind, const char *tool_id) {
    for (guint i = 0; i < s->observers->len; i++) {
        Obs *o = g_ptr_array_index(s->observers, i);
        o->fn(s, kind, tool_id, o->data);
    }
    store_session_changed(s, kind);
}

/* ---------------- mutations ---------------- */

void session_append(Session *s, Message *m) {
    g_ptr_array_add(s->messages, m);
    s->updated_at = now_ts();
    JsonObject *ev = jo_new();
    jo_str(ev, "t", "message");
    jo_obj(ev, "m", message_to_json(m));
    persist(s, ev);
    session_notify(s, CHANGE_RESET, NULL);
}

void session_set_tool_run(Session *s, const char *id, ToolRun *r, gboolean do_persist) {
    if (do_persist) {
        JsonObject *ev = jo_new();
        jo_str(ev, "t", "toolRun");
        jo_str(ev, "id", id);
        jo_obj(ev, "run", run_to_json(r));
        persist(s, ev);
    }
    g_hash_table_replace(s->tool_runs, g_strdup(id), r);
    session_notify(s, CHANGE_TOOL, id);
}

void session_update_tool_progress(Session *s, const char *id, const char *progress) {
    ToolRun *r = g_hash_table_lookup(s->tool_runs, id);
    if (!r) return;
    g_free(r->progress);
    r->progress = g_strdup(progress);
    session_notify(s, CHANGE_TOOL, id);
}

TurnRecord *session_begin_turn(Session *s, int message_index) {
    TurnRecord *t = g_new0(TurnRecord, 1);
    TurnRecord *last = session_last_turn(s);
    t->index = (last ? last->index : 0) + 1;
    t->started_at = now_ts();
    t->message_index = message_index;
    g_ptr_array_add(s->turns, t);
    JsonObject *ev = jo_new();
    jo_str(ev, "t", "turnStart");
    jo_int(ev, "index", t->index);
    jo_dbl(ev, "startedAt", t->started_at);
    jo_int(ev, "messageIndex", message_index);
    persist(s, ev);
    session_notify(s, CHANGE_META, NULL);
    return t;
}

void session_end_turn(Session *s, int index, TurnEnd end, const char *error) {
    double now = now_ts();
    for (int i = s->turns->len - 1; i >= 0; i--) {
        TurnRecord *t = g_ptr_array_index(s->turns, i);
        if (t->index == index) {
            t->ended_at = now;
            t->end = end;
            g_free(t->error);
            t->error = g_strdup(error);
            break;
        }
    }
    JsonObject *ev = jo_new();
    jo_str(ev, "t", "turnEnd");
    jo_int(ev, "index", index);
    jo_dbl(ev, "endedAt", now);
    jo_str(ev, "end", end_raw(end) ? end_raw(end) : "stopped");
    if (error) jo_str(ev, "error", error);
    persist(s, ev);
    session_notify(s, CHANGE_RESET, NULL);
}

void session_set_title(Session *s, const char *t) {
    g_free(s->title);
    s->title = g_strdup(t);
    s->has_title = TRUE;
    JsonObject *ev = jo_new();
    jo_str(ev, "t", "title");
    jo_str(ev, "title", t);
    persist(s, ev);
    session_notify(s, CHANGE_META, NULL);
}

void session_set_todos(Session *s, GPtrArray *todos) {
    g_ptr_array_unref(s->todos);
    s->todos = todos;
    JsonObject *ev = jo_new();
    jo_str(ev, "t", "todos");
    JsonArray *a = json_array_new();
    for (guint i = 0; i < todos->len; i++) {
        TodoItem *t = g_ptr_array_index(todos, i);
        JsonObject *o = jo_new();
        jo_str(o, "content", t->content);
        jo_str(o, "status", t->status);
        json_array_add_object_element(a, o);
    }
    jo_arr(ev, "todos", a);
    persist(s, ev);
    session_notify(s, CHANGE_META, NULL);
}

void session_mark_observed(Session *s, const char *path) {
    if (g_hash_table_contains(s->observed, path)) return;
    g_hash_table_add(s->observed, g_strdup(path));
    JsonObject *ev = jo_new();
    jo_str(ev, "t", "observed");
    jo_str(ev, "path", path);
    persist(s, ev);
}

gboolean session_has_observed(Session *s, const char *path) { return g_hash_table_contains(s->observed, path); }

void session_set_compaction(Session *s, int index, const char *summary) {
    s->compaction_index = index;
    g_free(s->compaction_summary);
    s->compaction_summary = g_strdup(summary);
    JsonObject *ev = jo_new();
    jo_str(ev, "t", "compaction");
    jo_int(ev, "index", index);
    jo_str(ev, "summary", summary);
    persist(s, ev);
    session_notify(s, CHANGE_META, NULL);
}

void session_save_config(Session *s) {
    JsonObject *ev = jo_new();
    jo_str(ev, "t", "config");
    jo_str(ev, "model", s->model);
    jo_str(ev, "effort", effort_raw(s->effort));
    jo_str(ev, "permission", perm_raw(s->permission));
    persist(s, ev);
    session_notify(s, CHANGE_META, NULL);
}

void session_set_draft(Session *s, Draft *d) {
    draft_free(s->draft);
    s->draft = d;
    session_notify(s, CHANGE_DRAFT, NULL);
}

void session_repair_dangling(Session *s, gboolean do_persist) {
    if (!s->messages->len) return;
    Message *last = g_ptr_array_index(s->messages, s->messages->len - 1);
    if (last->role != ROLE_ASSISTANT) return;
    Message *m = NULL;
    for (guint i = 0; i < last->blocks->len; i++) {
        Block *b = g_ptr_array_index(last->blocks, i);
        if (b->type != BLK_TOOL_USE) continue;
        if (!m) { m = message_new(ROLE_USER); m->is_tool_results = TRUE; }
        message_add(m, block_tool_result(b->id, "Tool call was interrupted before it produced a result.", TRUE, NULL));
    }
    if (!m) return;
    g_ptr_array_add(s->messages, m);
    if (do_persist) {
        JsonObject *ev = jo_new();
        jo_str(ev, "t", "message");
        jo_obj(ev, "m", message_to_json(m));
        persist(s, ev);
    }
}

JsonArray *session_wire_messages(Session *s, const char *model) {
    JsonArray *out = json_array_new();
    guint start = 0;
    gboolean summary = FALSE;
    if (s->compaction_index >= 0 && (guint)s->compaction_index < s->messages->len) {
        start = s->compaction_index;
        Message *first = g_ptr_array_index(s->messages, start);
        summary = first->role == ROLE_USER;
    }
    for (guint i = start; i < s->messages->len; i++) {
        Message *m = g_ptr_array_index(s->messages, i);
        JsonObject *o = jo_new();
        jo_str(o, "role", m->role == ROLE_USER ? "user" : "assistant");
        JsonArray *content = json_array_new();
        if (i == start && summary) {
            char *t = g_strdup_printf("<conversation-summary>\nThe earlier part of this conversation was compacted. Summary:\n\n%s\n</conversation-summary>",
                                      s->compaction_summary ? s->compaction_summary : "");
            Block *b = block_text(t);
            json_array_add_object_element(content, block_wire(b));
            block_free(b);
            g_free(t);
        }
        gboolean same_model = m->model == NULL || !g_strcmp0(m->model, model);
        for (guint j = 0; j < m->blocks->len; j++) {
            Block *b = g_ptr_array_index(m->blocks, j);
            if (m->role == ROLE_ASSISTANT && b->type == BLK_THINKING && !(same_model && b->signature && *b->signature)) continue;
            json_array_add_object_element(content, block_wire(b));
        }
        if (json_array_get_length(content) == 0) {
            JsonObject *t = jo_new();
            jo_str(t, "type", "text");
            jo_str(t, "text", "(no content)");
            json_array_add_object_element(content, t);
        }
        jo_arr(o, "content", content);
        json_array_add_object_element(out, o);
    }
    return out;
}

/* ---------------- derived ---------------- */

Usage session_total_usage(Session *s) {
    Usage u = { 0 };
    for (guint i = 0; i < s->messages->len; i++) {
        Message *m = g_ptr_array_index(s->messages, i);
        if (m->has_usage) usage_add(&u, &m->usage);
    }
    return u;
}

gint64 session_last_prompt_tokens(Session *s) {
    for (int i = s->messages->len - 1; i >= 0; i--) {
        Message *m = g_ptr_array_index(s->messages, i);
        if (m->has_usage) return usage_prompt(&m->usage);
    }
    return 0;
}

int session_step_count(Session *s) {
    int n = 0;
    for (guint i = 0; i < s->messages->len; i++)
        if (((Message *)g_ptr_array_index(s->messages, i))->role == ROLE_ASSISTANT) n++;
    return n;
}

double session_output_rate(Session *s) {
    gint64 out = 0;
    double dur = 0;
    for (guint i = 0; i < s->messages->len; i++) {
        Message *m = g_ptr_array_index(s->messages, i);
        if (m->role == ROLE_ASSISTANT && m->duration > 0.2) {
            out += m->usage.output;
            dur += m->duration;
        }
    }
    return dur > 0 ? out / dur : -1;
}

double session_cache_hit_rate(Session *s) {
    Usage u = session_total_usage(s);
    gint64 p = usage_prompt(&u);
    return p > 0 ? (double)u.cache_read / p : -1;
}

ToolRun *session_tool_run(Session *s, const char *id) { return id ? g_hash_table_lookup(s->tool_runs, id) : NULL; }

TurnRecord *session_last_turn(Session *s) { return s->turns->len ? g_ptr_array_index(s->turns, s->turns->len - 1) : NULL; }

const Message *session_last_assistant_with_text(Session *s) {
    for (int i = s->messages->len - 1; i >= 0; i--) {
        Message *m = g_ptr_array_index(s->messages, i);
        if (m->role != ROLE_ASSISTANT) continue;
        for (guint j = 0; j < m->blocks->len; j++) {
            Block *b = g_ptr_array_index(m->blocks, j);
            if (b->type == BLK_TEXT && !str_blank(b->text)) return m;
        }
    }
    return NULL;
}

char *session_export_markdown(Session *s) {
    GString *out = g_string_new("");
    g_string_append_printf(out, "# %s\n\n_Workspace: %s_\n\n", s->title, s->cwd);
    for (guint i = 0; i < s->messages->len; i++) {
        Message *m = g_ptr_array_index(s->messages, i);
        for (guint j = 0; j < m->blocks->len; j++) {
            Block *b = g_ptr_array_index(m->blocks, j);
            if (m->role == ROLE_USER) {
                if (b->type == BLK_TEXT)
                    g_string_append_printf(out, "%s%s\n\n", m->is_tool_results ? "**User (steer):** " : "## User\n\n", b->text);
                else if (b->type == BLK_TOOL_RESULT) {
                    char *c = utf8_prefix(b->text, 4000);
                    g_string_append_printf(out, "<details><summary>Tool result%s</summary>\n\n```\n%s\n```\n</details>\n\n", b->is_error ? " (error)" : "", c);
                    g_free(c);
                }
            } else {
                if (b->type == BLK_TEXT) g_string_append_printf(out, "%s\n\n", b->text);
                else if (b->type == BLK_TOOL_USE) {
                    char *c = utf8_prefix(b->input, 300);
                    g_string_append_printf(out, "**%s** `%s`\n\n", b->name, c);
                    g_free(c);
                }
            }
        }
    }
    return g_string_free(out, FALSE);
}
