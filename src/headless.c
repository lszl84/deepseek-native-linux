/* Terminal driver used for testing the agent without the GUI. */
#include "agent.h"
#include "store.h"
#include "headless.h"
#include <stdio.h>
#include <string.h>

static GMainLoop *loop;
static GHashTable *printed;
static int exit_code;

static void on_change(Session *s, ChangeKind kind, const char *tool_id, gpointer data) {
    if (kind != CHANGE_TOOL || !tool_id) return;
    ToolRun *r = session_tool_run(s, tool_id);
    if (!r || (r->status != TOOL_DONE && r->status != TOOL_ERROR && r->status != TOOL_DENIED) || g_hash_table_contains(printed, tool_id)) return;
    g_hash_table_add(printed, g_strdup(tool_id));
    const char *name = "?", *input = "";
    for (int i = s->messages->len - 1; i >= 0; i--) {
        Message *m = g_ptr_array_index(s->messages, i);
        for (guint j = 0; j < m->blocks->len; j++) {
            Block *b = g_ptr_array_index(m->blocks, j);
            if (b->type == BLK_TOOL_USE && !g_strcmp0(b->id, tool_id)) { name = b->name; input = b->input; }
        }
    }
    char *in = utf8_prefix(input, 160);
    char *out = utf8_prefix(r->output ? r->output : "", 300);
    char *flat = str_replace(out, "\n", "⏎");
    printf("  ⏺ %s %s → %s: %s\n", name, in, tool_status_raw(r->status), flat);
    g_free(in);
    g_free(out);
    g_free(flat);
}

static void on_event(Session *s, gboolean attention, gpointer data) {
    Agent *a = s->agent;
    if (attention) {
        while (s->pending_approvals->len) {
            ApprovalRequest *r = g_ptr_array_index(s->pending_approvals, 0);
            printf("  [auto-approve] %s\n", r->title);
            agent_resolve_approval(a, r->id, DECIDE_ONCE);
        }
        while (s->pending_questions->len) {
            QuestionRequest *q = g_ptr_array_index(s->pending_questions, 0);
            JsonObject *ans = jo_new();
            for (guint i = 0; i < q->questions->len; i++) {
                Question *qq = g_ptr_array_index(q->questions, i);
                JsonArray *arr = json_array_new();
                json_array_add_string_element(arr, qq->labels->len ? g_ptr_array_index(qq->labels, 0) : "yes");
                jo_arr(ans, qq->id, arr);
            }
            printf("  [auto-answer] question\n");
            agent_resolve_question(a, q->id, ans);
        }
        return;
    }
    for (guint i = 0; i < s->messages->len; i++) {
        Message *m = g_ptr_array_index(s->messages, i);
        if (m->role != ROLE_ASSISTANT) continue;
        for (guint j = 0; j < m->blocks->len; j++) {
            Block *b = g_ptr_array_index(m->blocks, j);
            if (b->type == BLK_THINKING) {
                char *t = utf8_prefix(b->text, 200);
                char *f = str_replace(t, "\n", " ");
                printf("💭 %s\n", f);
                g_free(t);
                g_free(f);
            } else if (b->type == BLK_TEXT) {
                printf("🤖 %s\n", b->text);
            }
        }
    }
    Usage u = session_total_usage(s);
    TurnRecord *t = session_last_turn(s);
    const char *end = !t ? "?" : t->end == END_COMPLETED ? "completed" : t->end == END_STOPPED ? "stopped" : t->end == END_ERROR ? "error" : "?";
    printf("— turn end: %s%s%s · steps %d · in %" G_GINT64_FORMAT " cacheRead %" G_GINT64_FORMAT " out %" G_GINT64_FORMAT "\n", end,
           t && t->error ? " · " : "", t && t->error ? t->error : "", session_step_count(s), u.input, u.cache_read, u.output);
    exit_code = t && t->end == END_COMPLETED ? 0 : 1;
    /* A finished background job starts a follow-up turn; keep running until none are left. */
    if (!s->running && !(a && agent_has_running_jobs(a)) && !(a && a->pending_notices->len)) g_main_loop_quit(loop);
    else if (!s->running) printf("  [waiting for background jobs]\n");
}

static gboolean cancel_cb(gpointer p) {
    Agent *a = p;
    printf("  [cancel]\n");
    agent_cancel(a);
    return G_SOURCE_REMOVE;
}

int headless_run(const char *prompt, const char *cwd, const char *permission) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    shell_env_warm_up();
    store_init();
    printed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *dir = path_standardize(cwd);
    Session *s = session_new(dir);
    g_free(dir);
    s->ephemeral = TRUE;
    s->permission = permission ? perm_parse(permission) : PERM_WORKSPACE_WRITE;
    Agent *a = agent_new(s, 0);
    session_observe(s, on_change, NULL);
    agent_set_event_handler(on_event, NULL);
    loop = g_main_loop_new(NULL, FALSE);
    agent_submit(a, prompt, NULL);
    const char *c = g_getenv("DSN_CANCEL_AFTER");
    if (c && *c) g_timeout_add((guint)(g_ascii_strtod(c, NULL) * 1000), cancel_cb, a);
    g_main_loop_run(loop);
    return exit_code;
}
