/* Imports sessions written by the Node DeepSeek Harness: ~/.dsh/sessions/WORKSPACE/session-ID/session.v4.jsonl.zstd */
#include "importer.h"
#include "store.h"
#include <string.h>

static char *sessions_root(void) { return g_build_filename(g_get_home_dir(), ".dsh", "sessions", NULL); }

GPtrArray *harness_candidates(void) {
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    char *root = sessions_root();
    GDir *d = g_dir_open(root, 0, NULL);
    const char *ws;
    while (d && (ws = g_dir_read_name(d))) {
        char *wsp = g_build_filename(root, ws, NULL);
        GDir *sd = g_dir_open(wsp, 0, NULL);
        const char *sn;
        while (sd && (sn = g_dir_read_name(sd))) {
            if (!g_str_has_prefix(sn, "session-")) continue;
            char *f = g_build_filename(wsp, sn, "session.v4.jsonl.zstd", NULL);
            if (g_file_test(f, G_FILE_TEST_IS_REGULAR)) g_ptr_array_add(out, f);
            else g_free(f);
        }
        if (sd) g_dir_close(sd);
        g_free(wsp);
    }
    if (d) g_dir_close(d);
    g_free(root);
    return out;
}

static char *decompress(const char *path, gsize *len) {
    char *zstd_path = g_find_program_in_path("zstd");
    const char *zstd = zstd_path ? "zstd" : NULL;
    g_free(zstd_path);
    char *out = NULL;
    int status = 0;
    if (zstd) {
        char *argv[] = { "zstd", "-dc", (char *)path, NULL };
        gsize n = 0;
        if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, &status, NULL) &&
            g_spawn_check_wait_status(status, NULL) && out && *out) {
            n = strlen(out);
            *len = n;
            return out;
        }
        g_free(out);
        out = NULL;
    }
    char *node = g_find_program_in_path("node");
    if (node) {
        char *argv[] = { node, "-e", "process.stdout.write(require('zlib').zstdDecompressSync(require('fs').readFileSync(process.argv[1])))", (char *)path, NULL };
        if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, &status, NULL) && g_spawn_check_wait_status(status, NULL) &&
            out && *out) {
            *len = strlen(out);
            g_free(node);
            return out;
        }
        g_free(out);
        g_free(node);
    }
    return NULL;
}

typedef struct { int index; double started; int message_index; } ImpTurn;
typedef struct { int index; double ended; const char *end; char *error; } ImpEnd;

static Message *last_msg(GPtrArray *m) { return m->len ? g_ptr_array_index(m, m->len - 1) : NULL; }

static void append_user(GPtrArray *msgs, Block *b, double time, gboolean tool_results) {
    Message *last = last_msg(msgs);
    if (last && last->role == ROLE_USER) {
        message_add(last, b);
        if (tool_results) last->is_tool_results = TRUE;
        return;
    }
    Message *m = message_new(ROLE_USER);
    m->time = time;
    m->is_tool_results = tool_results;
    message_add(m, b);
    g_ptr_array_add(msgs, m);
}

static char *text_of(JsonArray *content) {
    GString *s = g_string_new("");
    for (guint i = 0; content && i < json_array_get_length(content); i++) {
        JsonNode *n = json_array_get_element(content, i);
        if (!JSON_NODE_HOLDS_OBJECT(n)) continue;
        JsonObject *o = json_node_get_object(n);
        if (g_strcmp0(jstr(o, "type"), "text") || !jstr(o, "text")) continue;
        if (s->len) g_string_append_c(s, '\n');
        g_string_append(s, jstr(o, "text"));
    }
    return g_string_free(s, FALSE);
}

typedef struct { IndexEntry e; } AddCtx;

static void add_main(gpointer p) {
    AddCtx *c = p;
    store_add_imported(c->e.id, c->e.cwd, c->e.title, c->e.created_at, c->e.updated_at);
}

static gboolean import_one(const char *path, const char *id) {
    gsize len = 0;
    char *data = decompress(path, &len);
    if (!data) return FALSE;
    char *cwd = NULL, *title = g_strdup("Imported session");
    double created = 0, last_time = 0;
    GPtrArray *msgs = g_ptr_array_new_with_free_func(message_free);
    GHashTable *runs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, tool_run_free);
    GArray *turns = g_array_new(FALSE, FALSE, sizeof(ImpTurn));
    GArray *ends = g_array_new(FALSE, FALSE, sizeof(ImpEnd));
    JsonArray *todos = NULL;
    JsonNode *todos_node = NULL;
    int turn_index = 0;
    gboolean any_events = FALSE;
    JsonParser *parser = json_parser_new();
    char *p = data, *end = data + len;
    while (p < end) {
        char *nl = memchr(p, '\n', end - p);
        if (!nl) nl = end;
        if (nl > p && json_parser_load_from_data(parser, p, nl - p, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                JsonObject *e = json_node_get_object(root);
                const char *type = jstr(e, "type") ? jstr(e, "type") : "";
                double time = jdbl(e, "time", 0) / 1000.0;
                if (time > 0) last_time = time;
                JsonObject *d = jobj(e, "data");
                if (!strcmp(type, "session")) {
                    g_free(cwd);
                    cwd = g_strdup(jstr(e, "cwd"));
                    created = jdbl(e, "createdAt", 0) / 1000.0;
                } else if (!strcmp(type, "user/message")) {
                    char *text = text_of(jarr(d, "content"));
                    if (*text) {
                        Message *last = last_msg(msgs);
                        if (!last || last->role != ROLE_USER || !any_events) {
                            Message *m = message_new(ROLE_USER);
                            m->time = time;
                            message_add(m, block_text(text));
                            g_ptr_array_add(msgs, m);
                        } else append_user(msgs, block_text(text), time, FALSE);
                    }
                    g_free(text);
                } else if (!strcmp(type, "turn/start")) {
                    turn_index++;
                    for (int i = msgs->len - 1; i >= 0; i--) {
                        Message *m = g_ptr_array_index(msgs, i);
                        if (m->role == ROLE_USER && !m->is_tool_results) {
                            ImpTurn t = { turn_index, time, i };
                            g_array_append_val(turns, t);
                            any_events = TRUE;
                            break;
                        }
                    }
                } else if (!strcmp(type, "turn/end")) {
                    JsonObject *reason = jobj(d, "reason");
                    const char *kind = jstr(reason, "kind") ? jstr(reason, "kind") : "completed";
                    ImpEnd x = { turn_index, time, !strcmp(kind, "completed") ? "completed" : !strcmp(kind, "error") ? "error" : "stopped",
                                 g_strdup(jstr(reason, "message")) };
                    g_array_append_val(ends, x);
                    any_events = TRUE;
                } else if (!strcmp(type, "assistant/message")) {
                    JsonObject *m = jobj(d, "message");
                    if (!m) goto next;
                    Message *am = message_new(ROLE_ASSISTANT);
                    am->time = time;
                    JsonArray *content = jarr(m, "content");
                    for (guint i = 0; content && i < json_array_get_length(content); i++) {
                        JsonNode *bn = json_array_get_element(content, i);
                        if (!JSON_NODE_HOLDS_OBJECT(bn)) continue;
                        JsonObject *b = json_node_get_object(bn);
                        const char *bt = jstr(b, "type");
                        if (!g_strcmp0(bt, "text") && !str_blank(jstr(b, "text"))) message_add(am, block_text(jstr(b, "text")));
                        else if (!g_strcmp0(bt, "reasoning") && !str_blank(jstr(b, "text"))) message_add(am, block_thinking(jstr(b, "text"), NULL));
                        else if (!g_strcmp0(bt, "tool-call")) {
                            char *cid = jstr(b, "id") ? g_strdup(jstr(b, "id")) : uuid_new();
                            message_add(am, block_tool_use(cid, jstr(b, "name") ? jstr(b, "name") : "tool", jstr(b, "arguments") ? jstr(b, "arguments") : "{}"));
                            ToolRun *r = tool_run_new(TOOL_DONE);
                            r->started_at = time;
                            g_hash_table_replace(runs, cid, r);
                        }
                    }
                    JsonObject *u = jobj(d, "usage");
                    am->has_usage = TRUE;
                    am->usage.input = jint(u, "inputTokens", 0);
                    am->usage.output = jint(u, "outputTokens", 0);
                    am->usage.cache_read = jint(u, "cacheReadTokens", 0);
                    am->usage.cache_write = jint(u, "cacheWriteTokens", 0);
                    am->model = g_strdup(jstr(jobj(m, "source"), "model"));
                    g_ptr_array_add(msgs, am);
                } else if (!strcmp(type, "tool/result")) {
                    JsonObject *m = jobj(d, "message");
                    const char *cid = jstr(m, "toolCallId");
                    if (!cid) goto next;
                    char *text = text_of(jarr(m, "content"));
                    gboolean is_error = jbool(m, "isError", FALSE);
                    append_user(msgs, block_tool_result(cid, text, is_error, NULL), time, TRUE);
                    ToolRun *r = g_hash_table_lookup(runs, cid);
                    if (!r) { r = tool_run_new(TOOL_DONE); g_hash_table_replace(runs, g_strdup(cid), r); }
                    r->status = is_error ? TOOL_ERROR : TOOL_DONE;
                    r->finished_at = time;
                    g_free(r->output);
                    r->output = utf8_prefix(text, 200000);
                    g_free(text);
                } else if (!strcmp(type, "session/title")) {
                    if (!str_blank(jstr(d, "title"))) { g_free(title); title = g_strdup(jstr(d, "title")); }
                } else if (!strcmp(type, "todo/write")) {
                    if (todos_node) json_node_unref(todos_node);
                    todos_node = d && json_object_has_member(d, "todos") ? json_node_copy(json_object_get_member(d, "todos")) : NULL;
                    todos = todos_node && JSON_NODE_HOLDS_ARRAY(todos_node) ? json_node_get_array(todos_node) : NULL;
                }
            }
        }
    next:
        p = nl + 1;
    }
    g_object_unref(parser);
    g_free(data);
    gboolean ok = FALSE;
    if (cwd && *cwd && msgs->len) {
        GString *out = g_string_new("");
#define LINE(o) do { char *_s = jobj_to_str(o); json_object_unref(o); g_string_append(out, _s); g_string_append_c(out, '\n'); g_free(_s); } while (0)
        JsonObject *h = jo_new();
        jo_str(h, "t", "header");
        jo_str(h, "id", id);
        jo_str(h, "cwd", cwd);
        jo_dbl(h, "createdAt", created);
        LINE(h);
        JsonObject *t = jo_new();
        jo_str(t, "t", "title");
        jo_str(t, "title", title);
        LINE(t);
        for (guint i = 0; i < msgs->len; i++) {
            JsonObject *o = jo_new();
            jo_str(o, "t", "message");
            jo_obj(o, "m", message_to_json(g_ptr_array_index(msgs, i)));
            LINE(o);
        }
        for (guint i = 0; i < turns->len; i++) {
            ImpTurn *tr = &g_array_index(turns, ImpTurn, i);
            JsonObject *o = jo_new();
            jo_str(o, "t", "turnStart");
            jo_int(o, "index", tr->index);
            jo_dbl(o, "startedAt", tr->started);
            jo_int(o, "messageIndex", tr->message_index);
            LINE(o);
        }
        for (guint i = 0; i < ends->len; i++) {
            ImpEnd *en = &g_array_index(ends, ImpEnd, i);
            JsonObject *o = jo_new();
            jo_str(o, "t", "turnEnd");
            jo_int(o, "index", en->index);
            jo_dbl(o, "endedAt", en->ended);
            jo_str(o, "end", en->end);
            if (en->error) jo_str(o, "error", en->error);
            LINE(o);
        }
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, runs);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            ToolRun *r = v;
            JsonObject *o = jo_new();
            jo_str(o, "t", "toolRun");
            jo_str(o, "id", k);
            JsonObject *ro = jo_new();
            jo_str(ro, "status", tool_status_raw(r->status));
            if (r->started_at > 0) jo_dbl(ro, "startedAt", r->started_at);
            if (r->finished_at > 0) jo_dbl(ro, "finishedAt", r->finished_at);
            if (r->output) jo_str(ro, "output", r->output);
            jo_obj(o, "run", ro);
            LINE(o);
        }
        if (todos) {
            JsonObject *o = jo_new();
            jo_str(o, "t", "todos");
            json_object_set_array_member(o, "todos", json_array_ref(todos));
            LINE(o);
        }
#undef LINE
        char *fn = g_strconcat(id, ".jsonl", NULL);
        char *dest = g_build_filename(sessions_dir(), fn, NULL);
        if (write_file_atomic(dest, out->str, out->len, 0600)) {
            AddCtx c = { { (char *)id, cwd, title, created, MAX(last_time, created) } };
            main_sync(add_main, &c);
            ok = TRUE;
        }
        g_free(dest);
        g_free(fn);
        g_string_free(out, TRUE);
    }
    for (guint i = 0; i < ends->len; i++) g_free(g_array_index(ends, ImpEnd, i).error);
    if (todos_node) json_node_unref(todos_node);
    g_array_unref(turns);
    g_array_unref(ends);
    g_hash_table_unref(runs);
    g_ptr_array_unref(msgs);
    g_free(cwd);
    g_free(title);
    return ok;
}

typedef struct { const char *id; gboolean exists; } ExistsCtx;

static void exists_main(gpointer p) {
    ExistsCtx *c = p;
    c->exists = store_entry(c->id) != NULL;
}

int harness_import_all(void) {
    GPtrArray *files = harness_candidates();
    int count = 0;
    for (guint i = 0; i < files->len; i++) {
        const char *f = g_ptr_array_index(files, i);
        char *dir = g_path_get_dirname(f);
        char *id = g_path_get_basename(dir);
        ExistsCtx ec = { id, FALSE };
        main_sync(exists_main, &ec);
        if (!ec.exists && import_one(f, id)) count++;
        g_free(id);
        g_free(dir);
    }
    g_ptr_array_unref(files);
    return count;
}
