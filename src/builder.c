#include "builder.h"
#include "presentation.h"
#include <string.h>

void item_spec_free(gpointer p) {
    ItemSpec *s = p;
    g_free(s->key);
    if (s->el) el_free(s->el);
    g_free(s);
}

TranscriptState *tstate_new(void) {
    TranscriptState *s = g_new0(TranscriptState, 1);
    s->expanded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    s->collapsed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    s->selected = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_hash_table_unref);
    s->effective_expanded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    return s;
}

void tstate_free(TranscriptState *s) {
    g_hash_table_unref(s->expanded);
    g_hash_table_unref(s->collapsed);
    g_hash_table_unref(s->selected);
    g_hash_table_unref(s->effective_expanded);
    g_free(s);
}

void tstate_reset(TranscriptState *s) {
    g_hash_table_remove_all(s->expanded);
    g_hash_table_remove_all(s->collapsed);
    g_hash_table_remove_all(s->selected);
    g_hash_table_remove_all(s->effective_expanded);
}

static GHashTable *opt_set(TranscriptState *s, const char *req, const char *q, gboolean create) {
    char *k = g_strconcat(req, "/", q, NULL);
    GHashTable *set = g_hash_table_lookup(s->selected, k);
    if (!set && create) {
        set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        g_hash_table_insert(s->selected, k, set);
    } else g_free(k);
    return set;
}

gboolean tstate_option_selected(TranscriptState *s, const char *req, const char *q, const char *label) {
    GHashTable *set = opt_set(s, req, q, FALSE);
    return set && g_hash_table_contains(set, label);
}

void tstate_toggle_option(TranscriptState *s, const char *req, const char *q, const char *label, gboolean multi) {
    GHashTable *set = opt_set(s, req, q, TRUE);
    if (multi) {
        if (g_hash_table_contains(set, label)) g_hash_table_remove(set, label);
        else g_hash_table_add(set, g_strdup(label));
    } else {
        g_hash_table_remove_all(set);
        g_hash_table_add(set, g_strdup(label));
    }
}

GPtrArray *tstate_options(TranscriptState *s, const char *req, const char *q) {
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GHashTable *set = opt_set(s, req, q, FALSE);
    if (set) {
        GHashTableIter it;
        gpointer k;
        g_hash_table_iter_init(&it, set);
        while (g_hash_table_iter_next(&it, &k, NULL)) g_ptr_array_add(out, g_strdup(k));
    }
    return out;
}

/* ======================= context ======================= */

typedef enum { SEG_THINKING, SEG_TOOL } SegKind;

typedef struct {
    SegKind kind;
    char *text; /* thinking */
    char *key;  /* thinking key / tool id */
    gboolean streaming;
    char *name, *input; /* tool */
} Seg;

static void seg_free(gpointer p) {
    Seg *s = p;
    g_free(s->text);
    g_free(s->key);
    g_free(s->name);
    g_free(s->input);
    g_free(s);
}

typedef struct {
    Session *s;
    TranscriptState *st;
    HaveItemFn have;
    gpointer have_data;
    GPtrArray *items;
    GPtrArray *group; /* Seg* */
    gboolean group_streaming;
    TurnRecord *turn;
    GPtrArray *turn_texts; /* borrowed char* */
    Usage turn_usage;
    GPtrArray *presented; /* char* pairs path, description */
} Ctx;

static gboolean push(Ctx *c, const char *key, Sig sig, double gap, El **el_out) {
    ItemSpec *spec = g_new0(ItemSpec, 1);
    spec->key = g_strdup(key);
    spec->sig = sig;
    spec->gap = gap;
    g_ptr_array_add(c->items, spec);
    if (c->have && c->have(key, sig, c->have_data)) { *el_out = NULL; return FALSE; }
    *el_out = NULL;
    return TRUE;
}

static void set_el(Ctx *c, El *el) {
    ItemSpec *spec = g_ptr_array_index(c->items, c->items->len - 1);
    spec->el = el;
}

#define ADD_ITEM(key, sig, gap, EXPR) do { El *_e; if (push(c, key, sig, gap, &_e)) set_el(c, (EXPR)); } while (0)

static double gap_first(Ctx *c, double g) { return c->items->len == 0 ? 8 : g; }

/* ======================= simple items ======================= */

static void add_user_text(Ctx *c, const char *text, double time, const char *key, gboolean notice) {
    char *t = fmt_time(time);
    Sig sig = sig_str(sig_str(sig_init(), text), t);
    ADD_ITEM(key, sig, gap_first(c, 28), el_user_bubble(text, t, key, FALSE, notice));
    g_free(t);
}

static void add_user(Ctx *c, Message *m) {
    GString *imgs = g_string_new("");
    for (guint i = 0; i < m->blocks->len; i++) {
        Block *b = g_ptr_array_index(m->blocks, i);
        if (b->type != BLK_IMAGE) continue;
        ImageData *d = b->images && b->images->len ? g_ptr_array_index(b->images, 0) : NULL;
        g_string_append_printf(imgs, "%s🖼 %s", imgs->len ? "  " : "", d && d->path ? path_base(d->path) : "image");
    }
    char *text = message_text(m);
    if (imgs->len) {
        char *t = *text ? g_strconcat(imgs->str, "\n", text, NULL) : g_strdup(imgs->str);
        g_free(text);
        text = t;
    }
    add_user_text(c, text, m->time, m->id, FALSE);
    g_free(text);
    g_string_free(imgs, TRUE);
}

static char *notice_attr(const char *text, const char *name) {
    char *pat = g_strconcat(name, "=\"", NULL);
    const char *p = strstr(text, pat);
    g_free(pat);
    if (!p) return NULL;
    p += strlen(name) + 2;
    const char *e = strchr(p, '"');
    return e ? g_strndup(p, e - p) : NULL;
}

static void add_notice(Ctx *c, const char *text, const char *key) {
    char *title = g_strdup("Notice"), *sub = g_strdup(text);
    if (g_str_has_prefix(text, "<background-job-finished")) {
        char *id = notice_attr(text, "id"), *status = notice_attr(text, "status");
        g_free(title);
        title = g_strdup_printf("Background job %s %s", id ? id : "", status ? status : "finished");
        const char *s = strchr(text, '>'), *e = strstr(text, "</background-job-finished>");
        if (s && e && e > s) { g_free(sub); sub = g_strndup(s + 1, e - s - 1); }
        g_free(id);
        g_free(status);
    }
    Sig sig = sig_str(sig_str(sig_init(), title), sub);
    Trailing tr = { TR_NONE };
    ADD_ITEM(key, sig, 20, el_row("alarm-symbolic", title, sub, tr, key, (Hit){ 0 }, -1, C_TEXT3, F_SMALL, 26));
    g_free(title);
    g_free(sub);
}

static void add_header(Ctx *c, TurnRecord *t) {
    Session *s = c->s;
    gboolean running = t->end == END_NONE && s->running;
    char *label;
    ColorId color = C_TEXT3;
    double live = 0;
    switch (t->end) {
    case END_NONE:
        label = g_strdup(running ? "Working" : "Stopped");
        if (running) live = t->started_at;
        if (running && s->status_line) { g_free(label); label = g_strdup(s->status_line); color = C_WARNING; live = 0; }
        break;
    case END_COMPLETED: {
        char *d = fmt_duration((t->ended_at > 0 ? t->ended_at : t->started_at) - t->started_at);
        label = g_strdup_printf("Took %s", d);
        g_free(d);
        break;
    }
    case END_STOPPED: label = g_strdup("Stopped"); break;
    default: {
        char *d = fmt_duration((t->ended_at > 0 ? t->ended_at : t->started_at) - t->started_at);
        label = g_strdup_printf("Failed after %s", d);
        g_free(d);
        color = C_ERROR;
        break;
    }
    }
    char *key = g_strdup_printf("h%d", t->index);
    Sig sig = sig_dbl(sig_int(sig_int(sig_str(sig_init(), label), running), color), live);
    ADD_ITEM(key, sig, 20, el_turn_header(label, color, running, live));
    g_free(key);
    g_free(label);
}

/* ======================= tool details ======================= */

static El *mono(const char *s, ColorId col) { return el_mono(s, col, F_MONO_SMALL); }

/* Keeps the last max_lines lines; returns hidden count. */
static char *tail_lines(const char *s, int max_lines, gboolean show_all, int *hidden) {
    char *t = g_strdup(s ? s : "");
    size_t n = strlen(t);
    while (n && t[n - 1] == '\n') t[--n] = 0;
    char *start = t;
    while (*start == '\n') start++;
    *hidden = 0;
    if (show_all) {
        char *r = utf8_suffix(start, 200000);
        g_free(t);
        return r;
    }
    int count = 0;
    for (char *p = start + strlen(start); p > start; p--) {
        if (p[-1] == '\n') {
            count++;
            if (count >= max_lines) {
                int hid = 1;
                for (char *q = start; q < p - 1; q++)
                    if (*q == '\n') hid++;
                *hidden = hid;
                char *r = g_strdup(p);
                g_free(t);
                return r;
            }
        }
    }
    char *r = g_strdup(start);
    g_free(t);
    return r;
}

static char *head_lines(const char *s, int max_lines, gboolean show_all, int *hidden) {
    char *t = g_strdup(s ? s : "");
    size_t n = strlen(t);
    while (n && t[n - 1] == '\n') t[--n] = 0;
    char *start = t;
    while (*start == '\n') start++;
    *hidden = 0;
    int total = count_lines(start);
    if (show_all || total <= max_lines) {
        char *r = g_strdup(start);
        g_free(t);
        return r;
    }
    char *p = start;
    for (int i = 0; i < max_lines && p; i++) {
        p = strchr(p, '\n');
        if (p) p++;
    }
    char *r = p ? g_strndup(start, p - 1 - start) : g_strdup(start);
    *hidden = total - max_lines;
    g_free(t);
    return r;
}

static El *more_row(int hidden, const char *id, gboolean show_all, gboolean above) {
    char *key = g_strconcat("all:", id, NULL);
    El *e = NULL;
    Trailing tr = { TR_NONE };
    if (show_all) e = el_row("pan-up-symbolic", "Show less", "", tr, key, hit_make(HIT_TOGGLE, key, NULL, NULL, 0), -1, C_ACCENT, F_CAPTION, 22);
    else if (hidden > 0) {
        char *t = g_strdup_printf("%d %s lines", hidden, above ? "earlier" : "more");
        e = el_row("view-more-horizontal-symbolic", t, "", tr, key, hit_make(HIT_TOGGLE, key, NULL, NULL, 0), -1, C_ACCENT, F_CAPTION, 22);
        g_free(t);
    }
    g_free(key);
    return e;
}

static El *small_md_box(const char *md, const char *key, const char *cwd) {
    MDStyle st = md_style_body(cwd);
    st.font = F_SMALL;
    st.spacing = 4;
    return el_box(md_element(md, &st, key), 10, 12, 10, 12, C_CODE_BG, C_CODE_BORDER, 10);
}

static void split_lines(const char *s, GPtrArray *out) {
    char **l = g_strsplit(s ? s : "", "\n", -1);
    for (int i = 0; l[i]; i++) g_ptr_array_add(out, l[i]);
    g_free(l);
}

static El *tool_details(const char *name, JsonObject *in, const char *raw, ToolRun *run, const char *id, const char *cwd, gboolean show_all) {
    gboolean is_error = run && (run->status == TOOL_ERROR || run->status == TOOL_DENIED);
    const char *output = run ? (run->output ? run->output : run->progress ? run->progress : "") : "";
    ColorId out_color = is_error ? C_ERROR : C_TEXT2;
    GPtrArray *ch = g_ptr_array_new();
    int hidden = 0;
    if (!strcmp(name, "bash")) {
        char *cmd = g_strconcat("$ ", jstr(in, "command") ? jstr(in, "command") : "", NULL);
        g_ptr_array_add(ch, mono(cmd, C_TEXT));
        g_free(cmd);
        if (*output) {
            char *t = tail_lines(output, 40, show_all, &hidden);
            El *m = more_row(hidden, id, show_all, TRUE);
            if (m) g_ptr_array_add(ch, m);
            g_ptr_array_add(ch, mono(t, out_color));
            g_free(t);
        } else if (run && run->status == TOOL_RUNNING) g_ptr_array_add(ch, mono("Running…", C_TEXT3));
        return el_detail_box(ch);
    }
    if (!strcmp(name, "edit")) {
        GPtrArray *ol = g_ptr_array_new_with_free_func(g_free), *nl = g_ptr_array_new_with_free_func(g_free);
        split_lines(jstr(in, "old_string"), ol);
        split_lines(jstr(in, "new_string"), nl);
        guint p = 0;
        while (p < ol->len && p < nl->len && !strcmp(g_ptr_array_index(ol, p), g_ptr_array_index(nl, p))) p++;
        guint s = 0;
        while (s < ol->len - p && s < nl->len - p && !strcmp(g_ptr_array_index(ol, ol->len - 1 - s), g_ptr_array_index(nl, nl->len - 1 - s))) s++;
        GPtrArray *lines = g_ptr_array_new();
        GArray *kinds = g_array_new(FALSE, FALSE, sizeof(int));
        int k0 = 0, km = -1, kp = 1;
        for (guint i = p >= 2 ? p - 2 : 0; i < p; i++) { g_ptr_array_add(lines, g_strdup(g_ptr_array_index(ol, i))); g_array_append_val(kinds, k0); }
        for (guint i = p; i < ol->len - s; i++) { g_ptr_array_add(lines, g_strdup(g_ptr_array_index(ol, i))); g_array_append_val(kinds, km); }
        for (guint i = p; i < nl->len - s; i++) { g_ptr_array_add(lines, g_strdup(g_ptr_array_index(nl, i))); g_array_append_val(kinds, kp); }
        for (guint i = ol->len - s; i < MIN(ol->len, ol->len - s + 2); i++) { g_ptr_array_add(lines, g_strdup(g_ptr_array_index(ol, i))); g_array_append_val(kinds, k0); }
        guint limit = show_all ? 2000 : 80;
        guint total = lines->len;
        while (lines->len > limit) { g_free(g_ptr_array_index(lines, lines->len - 1)); g_ptr_array_remove_index(lines, lines->len - 1); }
        g_array_set_size(kinds, lines->len);
        g_ptr_array_add(ch, el_diff(lines, kinds));
        El *m = more_row(total > limit ? total - limit : 0, id, show_all && total > 80, FALSE);
        if (m) g_ptr_array_add(ch, m);
        if (is_error) g_ptr_array_add(ch, mono(output, C_ERROR));
        g_ptr_array_unref(ol);
        g_ptr_array_unref(nl);
        return el_vstack(ch, 6);
    }
    if (!strcmp(name, "write")) {
        const char *content = jstr(in, "content") ? jstr(in, "content") : "";
        char *t = head_lines(content, 60, show_all, &hidden);
        GPtrArray *lines = g_ptr_array_new();
        GArray *kinds = g_array_new(FALSE, FALSE, sizeof(int));
        char **l = g_strsplit(t, "\n", -1);
        int kp = 1;
        for (int i = 0; l[i]; i++) { g_ptr_array_add(lines, l[i]); g_array_append_val(kinds, kp); }
        g_free(l);
        g_free(t);
        g_ptr_array_add(ch, el_diff(lines, kinds));
        El *m = more_row(hidden, id, show_all && count_lines(content) > 60, FALSE);
        if (m) g_ptr_array_add(ch, m);
        if (is_error) g_ptr_array_add(ch, mono(output, C_ERROR));
        return el_vstack(ch, 6);
    }
    if (!strcmp(name, "todo_write")) {
        GPtrArray *markers = g_ptr_array_new();
        GArray *tasks = g_array_new(FALSE, FALSE, sizeof(int));
        GPtrArray *contents = g_ptr_array_new();
        JsonArray *a = jarr(in, "todos");
        for (guint i = 0; a && i < json_array_get_length(a); i++) {
            JsonNode *n = json_array_get_element(a, i);
            if (!JSON_NODE_HOLDS_OBJECT(n)) continue;
            JsonObject *t = json_node_get_object(n);
            const char *status = jstr(t, "status") ? jstr(t, "status") : "pending";
            gboolean done = !strcmp(status, "completed");
            RichText *rt = rt_plain(jstr(t, "content") ? jstr(t, "content") : "", F_BODY,
                                    (!strcmp(status, "in_progress") ? FS_SEMIBOLD : 0) | (done ? FS_STRIKE : 0), done ? C_TEXT3 : C_TEXT);
            g_ptr_array_add(markers, g_strdup(""));
            int task = done ? 1 : 0;
            g_array_append_val(tasks, task);
            g_ptr_array_add(contents, el_text(rt, 4));
        }
        g_ptr_array_unref(ch);
        return el_box(el_list(markers, tasks, contents, F_BODY, C_TEXT), 10, 12, 10, 12, C_CODE_BG, C_CODE_BORDER, 10);
    }
    if (!strcmp(name, "web_search")) {
        JsonObject *meta = run && run->meta && JSON_NODE_HOLDS_OBJECT(run->meta) ? json_node_get_object(run->meta) : NULL;
        JsonArray *src = jarr(meta, "sources");
        if (src && json_array_get_length(src)) {
            GString *md = g_string_new("");
            for (guint i = 0; i < json_array_get_length(src); i++) {
                JsonObject *o = json_array_get_object_element(src, i);
                g_string_append_printf(md, "%s- [%s](%s)", i ? "\n" : "", jstr(o, "title") ? jstr(o, "title") : "", jstr(o, "url") ? jstr(o, "url") : "");
            }
            El *e = small_md_box(md->str, id, cwd);
            g_string_free(md, TRUE);
            g_ptr_array_unref(ch);
            return e;
        }
        g_ptr_array_add(ch, mono(*output ? output : "Searching…", out_color));
        return el_detail_box(ch);
    }
    if (!strcmp(name, "subagent")) {
        char *prompt = head_lines(jstr(in, "prompt"), show_all ? 1000 : 6, FALSE, &hidden);
        g_ptr_array_add(ch, el_mono(prompt, C_TEXT3, F_CAPTION));
        g_free(prompt);
        if (run && run->output) {
            MDStyle st = md_style_body(cwd);
            st.font = F_SMALL;
            st.spacing = 5;
            char *t = head_lines(run->output, 40, show_all, &hidden);
            g_ptr_array_add(ch, el_rule(2));
            char *k = g_strconcat(id, ".r", NULL);
            g_ptr_array_add(ch, md_element(t, &st, k));
            g_free(k);
            g_free(t);
            El *m = more_row(hidden, id, show_all, FALSE);
            if (m) g_ptr_array_add(ch, m);
        } else if (run && run->progress) {
            g_ptr_array_add(ch, el_mono(run->progress, C_TEXT2, F_CAPTION));
        }
        return el_box(el_vstack(ch, 8), 10, 12, 10, 12, C_CODE_BG, C_CODE_BORDER, 10);
    }
    if (!strcmp(name, "ask_user_question")) {
        GString *md = g_string_new("");
        JsonArray *qs = jarr(in, "questions");
        for (guint i = 0; qs && i < json_array_get_length(qs); i++) {
            JsonObject *q = json_array_get_object_element(qs, i);
            g_string_append_printf(md, "**%s**\n\n", q && jstr(q, "question") ? jstr(q, "question") : "");
        }
        JsonObject *meta = run && run->meta && JSON_NODE_HOLDS_OBJECT(run->meta) ? json_node_get_object(run->meta) : NULL;
        JsonObject *answers = jobj(meta, "answers");
        if (answers) {
            GList *keys = json_object_get_members(answers);
            keys = g_list_sort(keys, (GCompareFunc)strcmp);
            for (GList *l = keys; l; l = l->next) {
                JsonArray *a = jarr(answers, l->data);
                GString *v = g_string_new("");
                for (guint j = 0; a && j < json_array_get_length(a); j++) {
                    const char *s = json_node_get_string(json_array_get_element(a, j));
                    if (s) g_string_append_printf(v, "%s%s", v->len ? ", " : "", s);
                }
                g_string_append_printf(md, "- %s: %s\n", (char *)l->data, v->str);
                g_string_free(v, TRUE);
            }
            g_list_free(keys);
        } else if (*output) g_string_append(md, output);
        El *e = small_md_box(md->str, id, cwd);
        g_string_free(md, TRUE);
        g_ptr_array_unref(ch);
        return e;
    }
    if (!strcmp(name, "read") || !strcmp(name, "grep") || !strcmp(name, "glob") || !strcmp(name, "web_fetch") || !strcmp(name, "job_output") ||
        !strcmp(name, "job_list") || !strcmp(name, "read_image")) {
        const char *src = *output ? output : (run && run->status == TOOL_RUNNING ? "Running…" : "(no output)");
        char *t = head_lines(src, 30, show_all, &hidden);
        g_ptr_array_add(ch, mono(t, out_color));
        g_free(t);
        El *m = more_row(hidden, id, show_all, FALSE);
        if (m) g_ptr_array_add(ch, m);
        return el_detail_box(ch);
    }
    JsonNode *n = json_parse_str(raw, -1);
    char *pretty = json_to_str(n, TRUE);
    if (n) json_node_unref(n);
    g_ptr_array_add(ch, mono(pretty, C_TEXT3));
    g_free(pretty);
    char *t = head_lines(output, 30, show_all, &hidden);
    if (*t) g_ptr_array_add(ch, mono(t, out_color));
    g_free(t);
    El *m = more_row(hidden, id, show_all, FALSE);
    if (m) g_ptr_array_add(ch, m);
    return el_detail_box(ch);
}

/* ======================= groups ======================= */

static Trailing tool_trailing(const char *name, ToolRun *run, gboolean streaming_input) {
    Trailing t = { TR_NONE };
    if (!run) { if (streaming_input) t.kind = TR_SPINNER; return t; }
    switch (run->status) {
    case TOOL_PENDING:
    case TOOL_RUNNING: t.kind = TR_SPINNER; return t;
    case TOOL_AWAITING: t = (Trailing){ TR_TEXT, g_strdup("Needs approval"), C_WARNING }; return t;
    case TOOL_DENIED: t = (Trailing){ TR_TEXT, g_strdup("Denied"), C_ERROR }; return t;
    case TOOL_CANCELLED: t = (Trailing){ TR_TEXT, g_strdup("Cancelled"), C_TEXT3 }; return t;
    case TOOL_ERROR:
        if (!strcmp(name, "bash") && tool_meta_has(run, "exitCode")) t = (Trailing){ TR_TEXT, g_strdup_printf("exit %" G_GINT64_FORMAT, tool_meta_int(run, "exitCode", 0)), C_ERROR };
        else t = (Trailing){ TR_TEXT, g_strdup("Failed"), C_ERROR };
        return t;
    case TOOL_DONE:
        if (!strcmp(name, "edit") || !strcmp(name, "write")) {
            if (tool_meta_has(run, "added") && tool_meta_has(run, "removed")) {
                gboolean created = !strcmp(name, "write") && tool_meta_has(run, "created") && tool_meta_int(run, "created", 0);
                t.kind = TR_DIFF;
                t.added = tool_meta_int(run, "added", 0);
                t.removed = created ? 0 : tool_meta_int(run, "removed", 0);
            }
        } else if (!strcmp(name, "bash")) {
            gint64 code = tool_meta_int(run, "exitCode", 0);
            if (tool_meta_has(run, "exitCode") && code != 0) t = (Trailing){ TR_TEXT, g_strdup_printf("exit %" G_GINT64_FORMAT, code), C_ERROR };
            else if (tool_meta_str(run, "job")) t = (Trailing){ TR_TEXT, g_strdup(tool_meta_str(run, "job")), C_TEXT3 };
            else if (run->started_at > 0 && run->finished_at - run->started_at >= 1) t = (Trailing){ TR_TEXT, fmt_duration(run->finished_at - run->started_at), C_TEXT3 };
        } else if (!strcmp(name, "grep") || !strcmp(name, "glob")) {
            if (tool_meta_has(run, "count")) {
                gint64 n = tool_meta_int(run, "count", 0);
                const char *unit = !strcmp(name, "grep") ? (n == 1 ? "match" : "matches") : (n == 1 ? "file" : "files");
                t = (Trailing){ TR_TEXT, g_strdup_printf("%" G_GINT64_FORMAT " %s", n, unit), C_TEXT3 };
            }
        } else if (!strcmp(name, "read")) {
            if (tool_meta_has(run, "lines")) t = (Trailing){ TR_TEXT, g_strdup_printf("%" G_GINT64_FORMAT " lines", tool_meta_int(run, "lines", 0)), C_TEXT3 };
        } else if (!strcmp(name, "subagent")) {
            if (tool_meta_has(run, "steps")) t = (Trailing){ TR_TEXT, g_strdup_printf("%" G_GINT64_FORMAT " steps", tool_meta_int(run, "steps", 0)), C_TEXT3 };
        } else if (!strcmp(name, "todo_write")) {
            gint64 n = tool_meta_int(run, "added", 0);
            if (n > 0) t = (Trailing){ TR_TEXT, g_strdup_printf("%" G_GINT64_FORMAT " added", n), C_TEXT3 };
        } else if (!strcmp(name, "web_search")) {
            JsonObject *m = run->meta && JSON_NODE_HOLDS_OBJECT(run->meta) ? json_node_get_object(run->meta) : NULL;
            JsonArray *a = jarr(m, "sources");
            if (a) t = (Trailing){ TR_TEXT, g_strdup_printf("%u sources", json_array_get_length(a)), C_TEXT3 };
        }
        return t;
    }
    return t;
}

static gboolean st_has(GHashTable *h, const char *k) { return g_hash_table_contains(h, k); }

static void flush_group(Ctx *c) {
    if (!c->group->len) return;
    GPtrArray *segs = c->group;
    gboolean streaming = c->group_streaming;
    c->group = g_ptr_array_new_with_free_func(seg_free);
    c->group_streaming = FALSE;
    Session *s = c->s;
    const char *cwd = s->cwd;
    int ntools = 0;
    const char *first_tool = NULL;
    for (guint i = 0; i < segs->len; i++) {
        Seg *sg = g_ptr_array_index(segs, i);
        if (sg->kind == SEG_TOOL) { ntools++; if (!first_tool) first_tool = sg->key; }
    }
    Seg *s0 = g_ptr_array_index(segs, 0);
    char *key = g_strconcat("g.", first_tool ? first_tool : s0->key, NULL);
    TranscriptState *st = c->st;

    if (ntools == 0) {
        GString *text = g_string_new("");
        for (guint i = 0; i < segs->len; i++) {
            Seg *sg = g_ptr_array_index(segs, i);
            if (text->len) g_string_append(text, "\n\n");
            g_string_append(text, sg->text);
        }
        gboolean expanded = st_has(st->expanded, key);
        if (expanded) g_hash_table_add(st->effective_expanded, g_strdup(key));
        Sig sig = sig_int(sig_int(sig_int(sig_init(), text->len), streaming), expanded);
        El *e;
        if (push(c, key, sig, 12, &e)) {
            GPtrArray *ch = g_ptr_array_new();
            Trailing tr = { streaming ? TR_SPINNER : TR_NONE };
            g_ptr_array_add(ch, el_row("weather-clear-night-symbolic", streaming ? "Thinking" : "Analysis completed", "", tr, key,
                                       hit_make(HIT_TOGGLE, key, NULL, NULL, 0), expanded, C_TEXT2, F_BODY, 30));
            if (expanded && text->len) {
                MDStyle ts = md_style_thinking();
                char *k = g_strconcat(key, ".t", NULL);
                g_ptr_array_add(ch, el_indent(md_element(text->str, &ts, k), 24));
                g_free(k);
                g_ptr_array_add(ch, el_spacer(4));
            }
            set_el(c, el_vstack(ch, 4));
        }
        g_string_free(text, TRUE);
        g_free(key);
        g_ptr_array_unref(segs);
        return;
    }

    Sig status = sig_init();
    gboolean active = streaming, awaiting = FALSE;
    ToolCategory *cats = g_new(ToolCategory, ntools);
    int ci = 0;
    for (guint i = 0; i < segs->len; i++) {
        Seg *sg = g_ptr_array_index(segs, i);
        if (sg->kind != SEG_TOOL) { status = sig_int(status, strlen(sg->text)); continue; }
        ToolRun *r = session_tool_run(s, sg->key);
        status = sig_str(status, r ? tool_status_raw(r->status) : "none");
        status = sig_int(status, r && r->output ? strlen(r->output) : 0);
        status = sig_int(status, r && r->progress ? (gint64)g_str_hash(r->progress) : 0);
        status = sig_int(status, r && r->meta ? 1 : 0);
        if (r && r->meta) { char *m = json_to_str(r->meta, FALSE); status = sig_str(status, m); g_free(m); }
        status = sig_int(status, strlen(sg->input));
        if (r && (r->status == TOOL_RUNNING || r->status == TOOL_PENDING || r->status == TOOL_AWAITING)) active = TRUE;
        if (r && r->status == TOOL_AWAITING) awaiting = TRUE;
        cats[ci++] = tool_category(sg->name);
    }
    gboolean expanded = st_has(st->expanded, key) || ((active || awaiting) && !st_has(st->collapsed, key));
    if (expanded) g_hash_table_add(st->effective_expanded, g_strdup(key));
    Sig esig = sig_int(sig_init(), expanded);
    for (guint i = 0; i < segs->len; i++) {
        Seg *sg = g_ptr_array_index(segs, i);
        gboolean e;
        if (sg->kind == SEG_THINKING) e = st_has(st->expanded, sg->key);
        else {
            ToolRun *r = session_tool_run(s, sg->key);
            e = st_has(st->expanded, sg->key) || (r && r->status == TOOL_AWAITING && !st_has(st->collapsed, sg->key));
            char *ak = g_strconcat("all:", sg->key, NULL);
            gboolean all = st_has(st->expanded, ak);
            if (all) g_hash_table_add(st->effective_expanded, g_strdup(ak));
            esig = sig_int(esig, all);
            g_free(ak);
        }
        if (e) g_hash_table_add(st->effective_expanded, g_strdup(sg->key));
        esig = sig_int(esig, e);
        /* present tool results show as cards after the reply */
        if (sg->kind == SEG_TOOL && !strcmp(sg->name, "present")) {
            ToolRun *r = session_tool_run(s, sg->key);
            JsonObject *m = r && r->meta && JSON_NODE_HOLDS_OBJECT(r->meta) ? json_node_get_object(r->meta) : NULL;
            JsonArray *files = jarr(m, "files");
            for (guint k = 0; files && k < json_array_get_length(files); k++) {
                JsonObject *f = json_array_get_object_element(files, k);
                if (!f || !jstr(f, "path")) continue;
                g_ptr_array_add(c->presented, g_strdup(jstr(f, "path")));
                g_ptr_array_add(c->presented, g_strdup(jstr(f, "description") ? jstr(f, "description") : ""));
            }
        }
    }
    Sig sig = sig_int(sig_int(sig_int(sig_int(status, segs->len), active), esig), expanded);
    El *e;
    if (push(c, key, sig, 12, &e)) {
        char *title = tool_group_title(cats, ntools, active);
        GPtrArray *ch = g_ptr_array_new();
        Trailing tr = { active && !expanded ? TR_SPINNER : TR_NONE };
        g_ptr_array_add(ch, el_row(tool_group_icon(cats, ntools), title, "", tr, key, hit_make(HIT_TOGGLE, key, NULL, NULL, 0), expanded, C_TEXT2, F_BODY, 30));
        g_free(title);
        if (expanded) {
            for (guint i = 0; i < segs->len; i++) {
                Seg *sg = g_ptr_array_index(segs, i);
                if (sg->kind == SEG_THINKING) {
                    char *first = first_line(sg->text, 200);
                    char *ft = str_trim(first);
                    gboolean ex = st_has(st->expanded, sg->key);
                    Trailing ttr = { sg->streaming ? TR_SPINNER : TR_NONE };
                    g_ptr_array_add(ch, el_row("weather-clear-night-symbolic", "Think", ft, ttr, sg->key, hit_make(HIT_TOGGLE, sg->key, NULL, NULL, 0), ex, C_TEXT2,
                                               F_BODY, 30));
                    if (ex) {
                        MDStyle ts = md_style_thinking();
                        char *k = g_strconcat(sg->key, ".t", NULL);
                        g_ptr_array_add(ch, el_indent(md_element(sg->text, &ts, k), 24));
                        g_free(k);
                    }
                    g_free(first);
                    g_free(ft);
                } else {
                    JsonNode *n = json_parse_str(sg->input, -1);
                    JsonObject *in = n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
                    JsonObject *empty = in ? NULL : jo_new();
                    if (!in) in = empty;
                    ToolRun *r = session_tool_run(s, sg->key);
                    gboolean ex = g_hash_table_contains(st->effective_expanded, sg->key);
                    char *sum = tool_summary(sg->name, in, cwd);
                    g_ptr_array_add(ch, el_row(tool_icon(sg->name), tool_display_name(sg->name), sum, tool_trailing(sg->name, r, streaming && !r), sg->key,
                                               hit_make(HIT_TOGGLE, sg->key, NULL, NULL, 0), ex, C_TEXT2, F_BODY, 30));
                    g_free(sum);
                    if (ex) {
                        char *ak = g_strconcat("all:", sg->key, NULL);
                        g_ptr_array_add(ch, el_indent(tool_details(sg->name, in, sg->input, r, sg->key, cwd, st_has(st->expanded, ak)), 24));
                        g_ptr_array_add(ch, el_spacer(4));
                        g_free(ak);
                    }
                    if (empty) json_object_unref(empty);
                    if (n) json_node_unref(n);
                }
            }
        }
        set_el(c, el_vstack(ch, 2));
    }
    g_free(cats);
    g_free(key);
    g_ptr_array_unref(segs);
}

/* Byte offsets just after blank lines outside code fences; streamed text is split there so
 * finished paragraphs are parsed and laid out once. */
static GArray *split_points(const char *t) {
    GArray *pts = g_array_new(FALSE, FALSE, sizeof(int));
    gboolean in_fence = FALSE, prev_blank = FALSE;
    const char *p = t;
    size_t total = strlen(t);
    while (*p) {
        const char *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        const char *q = p;
        while (q < end && *q == ' ') q++;
        if (end - q >= 3 && (!strncmp(q, "```", 3) || !strncmp(q, "~~~", 3))) in_fence = !in_fence;
        gboolean blank = TRUE;
        for (const char *r = p; r < end; r++)
            if (*r != ' ' && *r != '\t' && *r != '\r') blank = FALSE;
        if (blank && !in_fence && !prev_blank && nl && (size_t)(nl + 1 - t) < total) {
            int off = nl + 1 - t;
            g_array_append_val(pts, off);
        }
        prev_blank = blank;
        if (!nl) break;
        p = nl + 1;
    }
    return pts;
}

typedef struct { int kind; const char *text; const char *id, *name, *input; } DBlock;

static void add_assistant_blocks(Ctx *c, const char *mid, DBlock *blocks, int n, gboolean streaming) {
    Session *s = c->s;
    for (int j = 0; j < n; j++) {
        DBlock *b = &blocks[j];
        char *key = g_strdup_printf("%s.%d", mid, j);
        gboolean is_last = j == n - 1;
        if (b->kind == BLK_TEXT) {
            if (str_blank(b->text)) { g_free(key); continue; }
            flush_group(c);
            g_ptr_array_add(c->turn_texts, (gpointer)b->text);
            gboolean live = streaming && is_last;
            MDStyle st = md_style_body(s->cwd);
            GArray *pts = live ? split_points(b->text) : NULL;
            if (pts && pts->len) {
                int prev = 0;
                for (guint k = 0; k <= pts->len; k++) {
                    gboolean tail = k == pts->len;
                    int end = tail ? (int)strlen(b->text) : g_array_index(pts, int, k);
                    char *chunk = g_strndup(b->text + prev, end - prev);
                    if (!str_blank(chunk)) {
                        char *ck = tail ? g_strconcat(key, ".tail", NULL) : g_strdup_printf("%s.c%u", key, k);
                        Sig sig = sig_int(sig_int(sig_int(sig_init(), prev), end - prev), tail ? 11 : 7);
                        ADD_ITEM(ck, sig, prev == 0 ? 14 : 12, md_element(chunk, &st, ck));
                        g_free(ck);
                    }
                    g_free(chunk);
                    prev = end;
                }
            } else {
                Sig sig = sig_int(sig_int(sig_init(), strlen(b->text)), live);
                ADD_ITEM(key, sig, 14, md_element(b->text, &st, key));
            }
            if (pts) g_array_unref(pts);
        } else if (b->kind == BLK_THINKING) {
            gboolean live = streaming && is_last;
            if (str_blank(b->text) && !live) { g_free(key); continue; }
            Seg *sg = g_new0(Seg, 1);
            sg->kind = SEG_THINKING;
            sg->text = g_strdup(b->text);
            sg->key = g_strdup(key);
            sg->streaming = live;
            g_ptr_array_add(c->group, sg);
            c->group_streaming = live;
        } else if (b->kind == BLK_TOOL_USE) {
            Seg *sg = g_new0(Seg, 1);
            sg->kind = SEG_TOOL;
            sg->key = g_strdup(b->id);
            sg->name = g_strdup(b->name);
            sg->input = g_strdup(b->input ? b->input : "");
            sg->text = g_strdup("");
            g_ptr_array_add(c->group, sg);
            c->group_streaming = streaming && is_last;
        }
        g_free(key);
    }
}

static void end_turn(Ctx *c, TurnRecord *t) {
    flush_group(c);
    for (guint i = 0; i + 1 < c->presented->len; i += 2) {
        const char *p = g_ptr_array_index(c->presented, i), *d = g_ptr_array_index(c->presented, i + 1);
        char *key = g_strconcat("present.", p, NULL);
        ADD_ITEM(key, sig_str(sig_str(sig_init(), p), d), 10, el_file_card(p, d, key));
        g_free(key);
    }
    g_ptr_array_set_size(c->presented, 0);
    if (t->end == END_NONE) return;
    if (t->end == END_ERROR) {
        char *key = g_strdup_printf("err%d", t->index);
        TurnRecord *last = session_last_turn(c->s);
        gboolean is_last = last && last->index == t->index;
        const char *msg = t->error ? t->error : "The turn failed.";
        El *e;
        if (push(c, key, sig_int(sig_str(sig_init(), msg), is_last), 12, &e)) {
            GPtrArray *ch = g_ptr_array_new();
            g_ptr_array_add(ch, el_text(rt_plain(msg, F_SMALL, 0, C_ERROR), 4));
            if (is_last) {
                GPtrArray *btns = g_ptr_array_new();
                char *rk = g_strconcat(key, ".retry", NULL);
                g_ptr_array_add(btns, el_button("Retry", "view-refresh-symbolic", BTN_SECONDARY, hit_make(HIT_RETRY, NULL, NULL, NULL, 0), rk));
                g_free(rk);
                g_ptr_array_add(ch, el_button_row(btns));
            }
            set_el(c, el_box(el_vstack(ch, 10), 12, 14, 12, 14, C_ERROR_SOFT, -1, 12));
        }
        g_free(key);
    }
    char *key = g_strdup_printf("f%d", t->index);
    const char *copy = c->turn_texts->len ? g_ptr_array_index(c->turn_texts, c->turn_texts->len - 1) : "";
    gint64 total = usage_total(&c->turn_usage);
    char *usage = NULL;
    if (total > 0) {
        char *tk = fmt_tokens(total);
        usage = g_strconcat(tk, " tok", NULL);
        g_free(tk);
    }
    char *time = fmt_time(t->ended_at > 0 ? t->ended_at : t->started_at);
    Sig sig = sig_str(sig_str(sig_int(sig_init(), strlen(copy)), usage ? usage : ""), time);
    ADD_ITEM(key, sig, 10, el_footer(copy, usage ? usage : "", time, key));
    g_free(usage);
    g_free(time);
    g_free(key);
}

static void add_pending(Ctx *c) {
    Session *s = c->s;
    for (guint i = 0; i < s->pending_approvals->len; i++) {
        ApprovalRequest *a = g_ptr_array_index(s->pending_approvals, i);
        char *key = g_strconcat("ap.", a->id, NULL);
        El *e;
        if (push(c, key, sig_int(sig_str(sig_init(), a->id), s->permission), 14, &e)) {
            GPtrArray *ch = g_ptr_array_new();
            char *hk = g_strconcat(key, ".h", NULL);
            char *sub = g_strconcat(perm_label(s->permission), " mode", NULL);
            Trailing tr = { TR_NONE };
            g_ptr_array_add(ch, el_row("security-medium-symbolic", "Approval required", sub, tr, hk, (Hit){ 0 }, -1, C_WARNING, F_SMALL, 22));
            g_free(sub);
            g_free(hk);
            g_ptr_array_add(ch, el_text(rt_plain(a->title, F_BODY, FS_SEMIBOLD, C_TEXT), 4));
            GPtrArray *dch = g_ptr_array_new();
            g_ptr_array_add(dch, el_text_max(rt_plain(a->detail, F_MONO_SMALL, 0, C_TEXT2), 3, 12));
            g_ptr_array_add(ch, el_box(el_vstack(dch, 0), 8, 10, 8, 10, C_CODE_BG, C_CODE_BORDER, 8));
            GPtrArray *btns = g_ptr_array_new();
            char *k1 = g_strconcat(key, ".once", NULL), *k2 = g_strconcat(key, ".session", NULL), *k3 = g_strconcat(key, ".deny", NULL);
            g_ptr_array_add(btns, el_button("Allow once", "check", BTN_PRIMARY, hit_make(HIT_APPROVE, a->id, NULL, NULL, DECIDE_ONCE), k1));
            g_ptr_array_add(btns, el_button("Allow for session", NULL, BTN_SECONDARY, hit_make(HIT_APPROVE, a->id, NULL, NULL, DECIDE_SESSION), k2));
            g_ptr_array_add(btns, el_button("Deny", NULL, BTN_DANGER, hit_make(HIT_APPROVE, a->id, NULL, NULL, DECIDE_DENY), k3));
            g_free(k1);
            g_free(k2);
            g_free(k3);
            g_ptr_array_add(ch, el_button_row(btns));
            set_el(c, el_box(el_vstack(ch, 10), 14, 16, 14, 16, C_CARD_BG, C_CARD_BORDER, 14));
        }
        g_free(key);
    }
    for (guint i = 0; i < s->pending_questions->len; i++) {
        QuestionRequest *q = g_ptr_array_index(s->pending_questions, i);
        char *key = g_strconcat("q.", q->id, NULL);
        Sig sig = sig_str(sig_init(), q->id);
        for (guint k = 0; k < q->questions->len; k++) {
            Question *qq = g_ptr_array_index(q->questions, k);
            for (guint o = 0; o < qq->labels->len; o++)
                sig = sig_int(sig, tstate_option_selected(c->st, q->id, qq->id, g_ptr_array_index(qq->labels, o)));
        }
        gboolean immediate = q->questions->len == 1 && !((Question *)g_ptr_array_index(q->questions, 0))->multi;
        El *e;
        if (push(c, key, sig, 14, &e)) {
            GPtrArray *ch = g_ptr_array_new();
            for (guint k = 0; k < q->questions->len; k++) {
                Question *qq = g_ptr_array_index(q->questions, k);
                if (k > 0) g_ptr_array_add(ch, el_spacer(6));
                if (qq->header && *qq->header) {
                    char *up = g_utf8_strup(qq->header, -1);
                    g_ptr_array_add(ch, el_text(rt_plain(up, F_TINY, FS_SEMIBOLD, C_TEXT3), 2));
                    g_free(up);
                }
                MDStyle qs = { F_BODY, FS_SEMIBOLD, C_TEXT, 5, s->cwd };
                g_ptr_array_add(ch, el_text(md_inline(qq->question, &qs), 5));
                for (guint o = 0; o < qq->labels->len; o++) {
                    const char *label = g_ptr_array_index(qq->labels, o);
                    char *ok = g_strdup_printf("%s/%s/%s", key, qq->id, label);
                    g_ptr_array_add(ch, el_option(label, g_ptr_array_index(qq->descs, o), tstate_option_selected(c->st, q->id, qq->id, label), qq->multi,
                                                  hit_make(HIT_CHOOSE, q->id, qq->id, label, 0), ok));
                    g_free(ok);
                }
            }
            g_ptr_array_add(ch, el_text(rt_plain("You can also type an answer in the message box.", F_CAPTION, 0, C_TEXT3), 2));
            GPtrArray *btns = g_ptr_array_new();
            if (!immediate) {
                char *sk = g_strconcat(key, ".submit", NULL);
                g_ptr_array_add(btns, el_button("Submit", "go-up-symbolic", BTN_PRIMARY, hit_make(HIT_SUBMIT, q->id, NULL, NULL, 0), sk));
                g_free(sk);
            }
            char *dk = g_strconcat(key, ".dismiss", NULL);
            g_ptr_array_add(btns, el_button("Dismiss", NULL, BTN_PLAIN, hit_make(HIT_DISMISS, q->id, NULL, NULL, 0), dk));
            g_free(dk);
            g_ptr_array_add(ch, el_button_row(btns));
            set_el(c, el_box(el_vstack(ch, 8), 14, 16, 14, 16, C_CARD_BG, C_CARD_BORDER, 14));
        }
        g_free(key);
    }
    for (guint i = 0; i < s->queued_inputs->len; i++) {
        const char *text = g_ptr_array_index(s->queued_inputs, i);
        char *key = g_strdup_printf("queued.%u", i);
        ADD_ITEM(key, sig_str(sig_init(), text), 16, el_user_bubble(text, "Queued", key, TRUE, FALSE));
        g_free(key);
    }
}

GPtrArray *transcript_build(Session *s, TranscriptState *st, HaveItemFn have, gpointer data) {
    Ctx cx = { 0 };
    Ctx *c = &cx;
    c->s = s;
    c->st = st;
    c->have = have;
    c->have_data = data;
    c->items = g_ptr_array_new_with_free_func(item_spec_free);
    c->group = g_ptr_array_new_with_free_func(seg_free);
    c->turn_texts = g_ptr_array_new();
    c->presented = g_ptr_array_new_with_free_func(g_free);
    g_hash_table_remove_all(st->effective_expanded);
    GHashTable *turns_by_start = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < s->turns->len; i++) {
        TurnRecord *t = g_ptr_array_index(s->turns, i);
        g_hash_table_insert(turns_by_start, GINT_TO_POINTER(t->message_index + 1), t);
    }
    for (guint i = 0; i < s->messages->len; i++) {
        Message *m = g_ptr_array_index(s->messages, i);
        if ((int)i == s->compaction_index) {
            char *k = g_strdup_printf("compact.%u", i);
            Trailing tr = { TR_NONE };
            ADD_ITEM(k, sig_int(sig_init(), 1), 24, el_row("view-paged-symbolic", "Earlier context was compacted into a summary", "", tr, k, (Hit){ 0 }, -1, C_TEXT3, F_SMALL, 26));
            g_free(k);
        }
        TurnRecord *t = g_hash_table_lookup(turns_by_start, GINT_TO_POINTER(i + 1));
        if (t) {
            if (c->turn) end_turn(c, c->turn);
            c->turn = t;
            g_ptr_array_set_size(c->turn_texts, 0);
            memset(&c->turn_usage, 0, sizeof c->turn_usage);
            if (m->notice) {
                char *txt = message_text(m);
                add_notice(c, txt, m->id);
                g_free(txt);
            } else add_user(c, m);
            add_header(c, t);
            continue;
        }
        if (m->role == ROLE_ASSISTANT) {
            DBlock *db = g_new0(DBlock, m->blocks->len);
            int n = 0;
            for (guint j = 0; j < m->blocks->len; j++) {
                Block *b = g_ptr_array_index(m->blocks, j);
                if (b->type != BLK_TEXT && b->type != BLK_THINKING && b->type != BLK_TOOL_USE) { db[n++] = (DBlock){ BLK_TEXT, "" }; continue; }
                db[n++] = (DBlock){ b->type, b->text ? b->text : "", b->id, b->name, b->input };
            }
            add_assistant_blocks(c, m->id, db, n, FALSE);
            g_free(db);
            if (m->has_usage) usage_add(&c->turn_usage, &m->usage);
        } else if (m->is_tool_results) {
            for (guint j = 0; j < m->blocks->len; j++) {
                Block *b = g_ptr_array_index(m->blocks, j);
                if (b->type != BLK_TEXT) continue;
                flush_group(c);
                char *k = g_strdup_printf("%s.%u", m->id, j);
                if (g_str_has_prefix(b->text, "<background-job-finished")) add_notice(c, b->text, k);
                else add_user_text(c, b->text, m->time, k, FALSE);
                g_free(k);
            }
        } else if (m->notice) {
            char *txt = message_text(m);
            add_notice(c, txt, m->id);
            g_free(txt);
        } else {
            flush_group(c);
            add_user(c, m);
        }
    }
    if (s->draft && s->running) {
        Draft *d = s->draft;
        DBlock *db = g_new0(DBlock, d->blocks->len + 1);
        int n = 0;
        for (guint j = 0; j < d->blocks->len; j++) {
            DraftBlock *b = g_ptr_array_index(d->blocks, j);
            db[n++] = (DBlock){ b->type, b->text->str, b->id, b->name, b->text->str };
            if (b->type == BLK_TOOL_USE) db[n - 1].text = "";
        }
        add_assistant_blocks(c, s->draft_id ? s->draft_id : "draft", db, n, TRUE);
        g_free(db);
    }
    if (c->turn) end_turn(c, c->turn);
    else flush_group(c);
    add_pending(c);
    g_hash_table_unref(turns_by_start);
    g_ptr_array_unref(c->group);
    g_ptr_array_unref(c->turn_texts);
    g_ptr_array_unref(c->presented);
    return c->items;
}
