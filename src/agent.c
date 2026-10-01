#include "agent.h"
#include "store.h"
#include "presentation.h"
#include <string.h>
#include <stdarg.h>
#include <math.h>

static GHashTable *live_agents; /* set of Agent* still allocated (main thread) */
static GMutex live_lock;
static AgentEventFn event_fn;
static gpointer event_data;

void agent_set_event_handler(AgentEventFn fn, gpointer data) {
    event_fn = fn;
    event_data = data;
}

static gboolean agent_alive(Agent *a) {
    g_mutex_lock(&live_lock);
    gboolean r = live_agents && g_hash_table_contains(live_agents, a);
    g_mutex_unlock(&live_lock);
    return r;
}

/* ---------------- tool results ---------------- */

ToolResult *tool_ok(const char *text) {
    ToolResult *r = g_new0(ToolResult, 1);
    r->text = g_strdup(text ? text : "");
    return r;
}

ToolResult *tool_err(const char *text) {
    ToolResult *r = tool_ok(text);
    r->is_error = TRUE;
    return r;
}

ToolResult *tool_errf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    ToolResult *r = tool_err(s);
    g_free(s);
    return r;
}

void tool_result_free(ToolResult *r) {
    if (!r) return;
    g_free(r->text);
    if (r->images) g_ptr_array_unref(r->images);
    if (r->meta) json_object_unref(r->meta);
    g_free(r);
}

/* ---------------- lifecycle ---------------- */

static void on_job_finish(const char *id, const char *kind, const char *state, const char *label, gpointer data);

Agent *agent_new(Session *s, int depth) {
    Agent *a = g_new0(Agent, 1);
    a->session = s;
    a->depth = depth;
    a->jobs = jobs_new();
    a->tools = tool_registry(depth);
    g_mutex_init(&a->lock);
    a->pending_notices = g_ptr_array_new_with_free_func(g_free);
    a->children = g_ptr_array_new();
    s->agent = a;
    jobs_set_on_finish(a->jobs, on_job_finish, a);
    g_mutex_lock(&live_lock);
    if (!live_agents) live_agents = g_hash_table_new(NULL, NULL);
    g_hash_table_add(live_agents, a);
    g_mutex_unlock(&live_lock);
    return a;
}

void agent_free(Agent *a) {
    if (!a) return;
    g_mutex_lock(&live_lock);
    g_hash_table_remove(live_agents, a);
    g_mutex_unlock(&live_lock);
    if (a->thread) { g_thread_join(a->thread); a->thread = NULL; }
    jobs_free(a->jobs);
    if (a->session && a->session->agent == a) a->session->agent = NULL;
    g_ptr_array_unref(a->tools);
    g_ptr_array_unref(a->pending_notices);
    g_ptr_array_unref(a->children);
    draft_free(a->pending_draft);
    g_free(a->parent_call_id);
    g_free(a->step_model);
    g_mutex_clear(&a->lock);
    g_free(a);
}

Agent *agent_for(Session *s) { return s->agent ? s->agent : agent_new(s, 0); }

gboolean agent_has_running_jobs(Agent *a) { return jobs_running_count(a->jobs) > 0 || jobs_thread_count(a->jobs) > 0; }

gboolean agent_cancelled(Agent *a) { return g_atomic_int_get(&a->cancelled) != 0; }

static void cancel_tree(Agent *a) {
    g_atomic_int_set(&a->cancelled, 1);
    g_mutex_lock(&a->lock);
    for (guint i = 0; i < a->children->len; i++) cancel_tree(g_ptr_array_index(a->children, i));
    g_mutex_unlock(&a->lock);
}

/* ---------------- turns ---------------- */

static void start_turn(Agent *a, Message *m);

typedef struct { char *sid; char *title; } TitleSet;

static void title_set_main(gpointer p) {
    TitleSet *t = p;
    Session *s = store_live(t->sid);
    if (s && t->title) session_set_title(s, t->title);
    g_free(t->sid);
    g_free(t->title);
    g_free(t);
}

typedef struct { char *sid; char *text; char *model; } TitleJob;

static gpointer title_thread(gpointer p) {
    TitleJob *j = p;
    TitleSet *t = g_new0(TitleSet, 1);
    t->sid = j->sid;
    t->title = llm_title(j->text, j->model);
    main_async(title_set_main, t);
    g_free(j->text);
    g_free(j->model);
    g_free(j);
    return NULL;
}

typedef struct { Agent *a; TurnEnd end; char *error; int turn; } FinishCtx;

static void finish_turn(gpointer p);

static gpointer loop_thread(gpointer p) {
    Agent *a = p;
    FinishCtx *f = g_new0(FinishCtx, 1);
    f->a = a;
    f->turn = a->turn_index;
    f->end = agent_loop(a, &f->error);
    main_async(finish_turn, f);
    return NULL;
}

static void start_turn(Agent *a, Message *m) {
    Session *s = a->session;
    session_repair_dangling(s, TRUE);
    gboolean first_human = !m->notice && !s->has_title;
    char *text = message_text(m);
    session_append(s, m);
    if (!s->ephemeral) {
        if (first_human) {
            char *fb = first_line(text, 50);
            char *t = str_trim(fb);
            session_set_title(s, *t ? t : "New Session");
            g_free(t);
            g_free(fb);
        }
        store_register(s);
        if (first_human) {
            TitleJob *j = g_new0(TitleJob, 1);
            j->sid = g_strdup(s->id);
            j->text = g_strdup(text);
            j->model = g_strdup(s->model);
            g_thread_unref(g_thread_new("title", title_thread, j));
        }
    }
    g_free(text);
    TurnRecord *t = session_begin_turn(s, s->messages->len - 1);
    a->turn_index = t->index;
    s->running = TRUE;
    g_clear_pointer(&s->last_error, g_free);
    g_atomic_int_set(&a->cancelled, 0);
    session_notify(s, CHANGE_META, NULL);
    if (a->thread) g_thread_join(a->thread);
    a->thread = g_thread_new("agent", loop_thread, a);
}

static void finish_turn(gpointer p) {
    FinishCtx *f = p;
    Agent *a = f->a;
    if (!agent_alive(a)) { g_free(f->error); g_free(f); return; }
    Session *s = a->session;
    if (a->thread) { g_thread_join(a->thread); a->thread = NULL; }
    s->running = FALSE;
    g_clear_pointer(&s->status_line, g_free);
    g_mutex_lock(&a->lock);
    g_clear_pointer(&a->pending_draft, draft_free);
    g_mutex_unlock(&a->lock);
    session_set_draft(s, NULL);
    for (guint i = 0; i < s->pending_approvals->len; i++) waiter_resolve(((ApprovalRequest *)g_ptr_array_index(s->pending_approvals, i))->waiter, DECIDE_DENY, NULL);
    g_ptr_array_set_size(s->pending_approvals, 0);
    for (guint i = 0; i < s->pending_questions->len; i++) waiter_resolve(((QuestionRequest *)g_ptr_array_index(s->pending_questions, i))->waiter, 0, NULL);
    g_ptr_array_set_size(s->pending_questions, 0);
    if (f->end == END_ERROR) { g_free(s->last_error); s->last_error = g_strdup(f->error); }
    session_end_turn(s, f->turn, f->end, f->end == END_ERROR ? f->error : NULL);
    TurnEnd end = f->end;
    g_free(f->error);
    g_free(f);
    if (s->deleted) {
        g_ptr_array_set_size(s->queued_inputs, 0);
        g_ptr_array_set_size(s->queued_images, 0);
        g_ptr_array_set_size(a->pending_notices, 0);
    } else if (s->queued_inputs->len || s->queued_images->len) {
        GString *t = g_string_new("");
        for (guint i = 0; i < s->queued_inputs->len; i++) {
            if (i) g_string_append(t, "\n\n");
            g_string_append(t, g_ptr_array_index(s->queued_inputs, i));
        }
        g_ptr_array_set_size(s->queued_inputs, 0);
        GPtrArray *imgs = s->queued_images;
        s->queued_images = g_ptr_array_new_with_free_func(image_data_free);
        agent_submit(a, t->str, imgs->len ? imgs : (g_ptr_array_unref(imgs), NULL));
        g_string_free(t, TRUE);
    } else if (a->pending_notices->len && end == END_COMPLETED) {
        GString *t = g_string_new("");
        for (guint i = 0; i < a->pending_notices->len; i++) {
            if (i) g_string_append(t, "\n\n");
            g_string_append(t, g_ptr_array_index(a->pending_notices, i));
        }
        g_ptr_array_set_size(a->pending_notices, 0);
        Message *m = message_new(ROLE_USER);
        m->notice = TRUE;
        message_add(m, block_text(t->str));
        g_string_free(t, TRUE);
        start_turn(a, m);
    }
    if (!s->ephemeral) store_emit(STORE_TURN_FINISHED, s);
    if (event_fn) event_fn(s, FALSE, event_data);
}

void agent_submit(Agent *a, const char *text, GPtrArray *images) {
    Session *s = a->session;
    if (s->running) {
        g_ptr_array_add(s->queued_inputs, g_strdup(text));
        for (guint i = 0; images && i < images->len; i++) g_ptr_array_add(s->queued_images, image_data_copy(g_ptr_array_index(images, i)));
        if (images) g_ptr_array_unref(images);
        session_notify(s, CHANGE_META, NULL);
        return;
    }
    Message *m = message_new(ROLE_USER);
    if (images) {
        for (guint i = 0; i < images->len; i++) message_add(m, block_image(image_data_copy(g_ptr_array_index(images, i))));
        g_ptr_array_unref(images);
    }
    message_add(m, block_text(text));
    start_turn(a, m);
}

void agent_cancel(Agent *a) {
    cancel_tree(a);
    Session *s = a->session;
    for (guint i = 0; i < s->pending_approvals->len; i++) waiter_resolve(((ApprovalRequest *)g_ptr_array_index(s->pending_approvals, i))->waiter, DECIDE_DENY, NULL);
    g_ptr_array_set_size(s->pending_approvals, 0);
    for (guint i = 0; i < s->pending_questions->len; i++) waiter_resolve(((QuestionRequest *)g_ptr_array_index(s->pending_questions, i))->waiter, 0, NULL);
    g_ptr_array_set_size(s->pending_questions, 0);
    session_notify(s, CHANGE_META, NULL);
}

/* ---------------- background job notices ---------------- */

typedef struct { Agent *a; char *text; } Notice;

static void deliver_notice(gpointer p) {
    Notice *n = p;
    Agent *a = n->a;
    if (agent_alive(a)) {
        if (a->session->running) {
            g_ptr_array_add(a->pending_notices, n->text);
            n->text = NULL;
        } else if (a->depth == 0 && !a->session->deleted) {
            Message *m = message_new(ROLE_USER);
            m->notice = TRUE;
            message_add(m, block_text(n->text));
            start_turn(a, m);
        }
    }
    g_free(n->text);
    g_free(n);
}

static void on_job_finish(const char *id, const char *kind, const char *state, const char *label, gpointer data) {
    Notice *n = g_new0(Notice, 1);
    n->a = data;
    n->text = g_strdup_printf("<background-job-finished id=\"%s\" kind=\"%s\" status=\"%s\">%s</background-job-finished>\n"
                              "Collect it with job_output if still relevant.", id, kind, state, label);
    main_async(deliver_notice, n);
}

/* ---------------- streaming draft plumbing ---------------- */

static gboolean flush_draft(gpointer p) {
    Agent *a = p;
    if (!agent_alive(a)) return G_SOURCE_REMOVE;
    g_mutex_lock(&a->lock);
    Draft *d = a->pending_draft ? draft_copy(a->pending_draft) : NULL;
    a->flush_scheduled = FALSE;
    g_mutex_unlock(&a->lock);
    if (d && a->session->running) session_set_draft(a->session, d);
    else draft_free(d);
    return G_SOURCE_REMOVE;
}

static void push_draft(const Draft *d, gpointer data) {
    Agent *a = data;
    Draft *c = draft_copy(d);
    g_mutex_lock(&a->lock);
    draft_free(a->pending_draft);
    a->pending_draft = c;
    gboolean schedule = !a->flush_scheduled;
    a->flush_scheduled = TRUE;
    g_mutex_unlock(&a->lock);
    if (schedule) g_timeout_add(50, flush_draft, a);
}

/* ---------------- main-thread helpers for the loop ---------------- */

typedef struct {
    Agent *a;
    char *system;
    char *body;
    char *model;
    gboolean ok;
} BuildCtx;

static JsonNode *tool_spec_node(const ToolDef *t) {
    JsonObject *o = jo_new();
    jo_str(o, "name", t->name);
    jo_str(o, "description", t->description);
    JsonNode *schema = json_parse_str(t->schema, -1);
    if (!schema) { schema = jnode_obj(jo_new()); json_object_unref(json_node_get_object(schema)); }
    json_object_set_member(o, "input_schema", schema);
    JsonNode *n = jnode_obj(o);
    json_object_unref(o);
    return n;
}

static void build_prompt_main(gpointer p) {
    BuildCtx *b = p;
    b->system = system_prompt_build(b->a->session, b->a->tools, b->a->depth);
}

static void build_body_main(gpointer p) {
    BuildCtx *b = p;
    Agent *a = b->a;
    Session *s = a->session;
    Settings *st = settings();
    JsonObject *o = jo_new();
    jo_str(o, "model", s->model);
    jo_bool(o, "stream", TRUE);
    jo_int(o, "max_tokens", st->max_tokens);
    jo_arr(o, "messages", session_wire_messages(s, s->model));
    JsonObject *th = jo_new();
    jo_str(th, "type", s->effort == EFFORT_OFF ? "disabled" : "enabled");
    jo_obj(o, "thinking", th);
    if (s->effort != EFFORT_OFF) {
        JsonObject *oc = jo_new();
        jo_str(oc, "effort", effort_raw(s->effort));
        jo_obj(o, "output_config", oc);
    }
    if (b->system && *b->system) jo_str(o, "system", b->system);
    if (a->tools->len) {
        JsonArray *arr = json_array_new();
        for (guint i = 0; i < a->tools->len; i++) json_array_add_element(arr, tool_spec_node(g_ptr_array_index(a->tools, i)));
        jo_arr(o, "tools", arr);
    }
    b->body = jobj_to_str(o);
    json_object_unref(o);
    g_free(b->model);
    b->model = g_strdup(s->model);
}

typedef struct { Agent *a; double started; char *id; } DraftStart;

static void draft_start_main(gpointer p) {
    DraftStart *d = p;
    Session *s = d->a->session;
    s->draft_started_at = d->started;
    g_free(s->draft_id);
    s->draft_id = g_strdup(d->id);
    session_set_draft(s, draft_new());
}

typedef struct { Agent *a; char *status; } StatusSet;

static void status_main(gpointer p) {
    StatusSet *x = p;
    Session *s = x->a->session;
    if (g_strcmp0(s->status_line, x->status)) {
        g_free(s->status_line);
        s->status_line = g_strdup(x->status);
        if (x->status) session_set_draft(s, draft_new());
        session_notify(s, CHANGE_META, NULL);
    }
}

static void set_status(Agent *a, const char *status) {
    StatusSet x = { a, (char *)status };
    main_sync(status_main, &x);
}

typedef struct { Agent *a; char *model; double started; char *id; } PartialCtx;

static void save_partial_main(gpointer p) {
    PartialCtx *c = p;
    Agent *a = c->a;
    Session *s = a->session;
    g_mutex_lock(&a->lock);
    Draft *d = a->pending_draft;
    a->pending_draft = NULL;
    g_mutex_unlock(&a->lock);
    g_clear_pointer(&s->draft, draft_free);
    if (!d) { session_set_draft(s, NULL); return; }
    Message *m = message_new(ROLE_ASSISTANT);
    g_free(m->id);
    m->id = g_strdup(c->id);
    for (guint i = 0; i < d->blocks->len; i++) {
        DraftBlock *b = g_ptr_array_index(d->blocks, i);
        if (b->type == BLK_TOOL_USE || !b->text->len) continue;
        /* A partial thinking block has no valid signature. */
        message_add(m, b->type == BLK_TEXT ? block_text(b->text->str) : block_thinking(b->text->str, NULL));
    }
    if (!m->blocks->len) {
        message_free(m);
        draft_free(d);
        session_set_draft(s, NULL);
        return;
    }
    m->has_usage = TRUE;
    m->usage = d->usage;
    m->model = g_strdup(c->model);
    m->duration = now_ts() - c->started;
    draft_free(d);
    session_append(s, m);
}

typedef struct { Agent *a; Message *m; } AppendCtx;

static void append_assistant_main(gpointer p) {
    AppendCtx *c = p;
    Agent *a = c->a;
    g_mutex_lock(&a->lock);
    g_clear_pointer(&a->pending_draft, draft_free);
    g_mutex_unlock(&a->lock);
    g_clear_pointer(&a->session->draft, draft_free);
    session_append(a->session, c->m);
}

static void append_main(gpointer p) {
    AppendCtx *c = p;
    session_append(c->a->session, c->m);
}

/* ---------------- retry ---------------- */

static Draft *stream_with_retry(Agent *a, const char *body, LLMError **err) {
    int attempt = 0;
    const char *sid = a->session->ephemeral ? NULL : a->session->id;
    for (;;) {
        LLMError *e = NULL;
        Draft *d = llm_stream(body, sid, &a->cancelled, push_draft, a, &e);
        if (d) {
            if (a->session->status_line) set_status(a, NULL);
            return d;
        }
        if (!e->retryable || e->cancelled || attempt >= 6 || agent_cancelled(a)) { *err = e; return NULL; }
        attempt++;
        double delay = e->retry_after > 0 ? e->retry_after : pow(2, attempt - 1) * 1.5;
        if (delay > 60) delay = 60;
        char *st = g_strdup_printf("Retrying in %ds (attempt %d of 6): %s", (int)delay, attempt, e->message);
        set_status(a, st);
        g_free(st);
        llm_error_free(e);
        gint64 end = g_get_monotonic_time() + (gint64)(delay * G_USEC_PER_SEC);
        while (g_get_monotonic_time() < end) {
            if (agent_cancelled(a)) {
                LLMError *c = g_new0(LLMError, 1);
                c->message = g_strdup("Cancelled");
                c->cancelled = TRUE;
                *err = c;
                return NULL;
            }
            g_usleep(100000);
        }
    }
}

/* ---------------- compaction ---------------- */

typedef struct {
    Agent *a;
    int cut;
    char *transcript;
    char *model;
} CompactCtx;

static void compact_check_main(gpointer p) {
    CompactCtx *c = p;
    Session *s = c->a->session;
    c->cut = -1;
    gint64 window = settings()->context_window;
    if (session_last_prompt_tokens(s) <= (gint64)(window * 0.8)) return;
    TurnRecord *t = session_last_turn(s);
    if (!t) return;
    int cut = t->message_index;
    if (s->compaction_index >= cut || cut <= 0) return;
    GString *tr = g_string_new("");
    int from = s->compaction_index >= 0 ? s->compaction_index : 0;
    if (s->compaction_summary && from > 0) g_string_append_printf(tr, "[earlier summary] %s\n", s->compaction_summary);
    for (int i = from; i < cut && i < (int)s->messages->len; i++) {
        Message *m = g_ptr_array_index(s->messages, i);
        const char *role = m->role == ROLE_USER ? "user" : "assistant";
        for (guint j = 0; j < m->blocks->len; j++) {
            Block *b = g_ptr_array_index(m->blocks, j);
            char *x = NULL;
            switch (b->type) {
            case BLK_TEXT: x = utf8_prefix(b->text, 8000); g_string_append_printf(tr, "\n[%s] %s", role, x); break;
            case BLK_TOOL_USE: x = utf8_prefix(b->input, 1500); g_string_append_printf(tr, "\n[tool call %s] %s", b->name, x); break;
            case BLK_TOOL_RESULT: x = utf8_prefix(b->text, 1500); g_string_append_printf(tr, "\n[tool result%s] %s", b->is_error ? " error" : "", x); break;
            default: break;
            }
            g_free(x);
        }
    }
    c->cut = cut;
    c->transcript = g_string_free(tr, FALSE);
    c->model = g_strdup(s->model);
    g_free(s->status_line);
    s->status_line = g_strdup("Compacting context…");
    session_notify(s, CHANGE_META, NULL);
}

typedef struct { Agent *a; int cut; char *summary; } CompactSet;

static void compact_set_main(gpointer p) {
    CompactSet *c = p;
    Session *s = c->a->session;
    if (c->summary && *c->summary) session_set_compaction(s, c->cut, c->summary);
    g_clear_pointer(&s->status_line, g_free);
    session_notify(s, CHANGE_META, NULL);
}

static void compact_if_needed(Agent *a) {
    CompactCtx c = { .a = a };
    main_sync(compact_check_main, &c);
    if (c.cut < 0) return;
    char *tail = utf8_suffix(c.transcript, 600000);
    char *prompt = g_strconcat("Summarize this coding-agent conversation so work can continue without it. Keep: the user's goals and constraints, "
                               "decisions made, files created or changed (with paths), commands and their important results, open problems, "
                               "and next steps. Be specific and concise.\n\n", tail, NULL);
    g_free(tail);
    JsonObject *o = jo_new();
    jo_str(o, "model", c.model);
    jo_int(o, "max_tokens", 8000);
    jo_bool(o, "stream", FALSE);
    JsonObject *th = jo_new();
    jo_str(th, "type", "disabled");
    jo_obj(o, "thinking", th);
    JsonArray *msgs = json_array_new();
    JsonObject *m = jo_new();
    jo_str(m, "role", "user");
    JsonArray *content = json_array_new();
    JsonObject *t = jo_new();
    jo_str(t, "type", "text");
    jo_str(t, "text", prompt);
    json_array_add_object_element(content, t);
    jo_arr(m, "content", content);
    json_array_add_object_element(msgs, m);
    jo_arr(o, "messages", msgs);
    char *body = jobj_to_str(o);
    json_object_unref(o);
    g_free(prompt);
    LLMError *err = NULL;
    JsonNode *resp = llm_complete(body, a->session->id, &a->cancelled, &err);
    g_free(body);
    CompactSet cs = { a, c.cut, resp ? llm_response_text(resp) : NULL };
    if (resp) json_node_unref(resp);
    llm_error_free(err);
    main_sync(compact_set_main, &cs);
    g_free(cs.summary);
    g_free(c.transcript);
    g_free(c.model);
}

/* ---------------- tool execution ---------------- */

typedef struct {
    char *id;
    char *name;
    char *input;
} Call;

typedef struct {
    Agent *a;
    Call *call;
    gboolean truncated;
    Block *result;
} ExecCtx;

typedef struct { Agent *a; const char *id; ToolStatus status; double started; } RunSet;

static void run_set_main(gpointer p) {
    RunSet *r = p;
    ToolRun *run = tool_run_new(r->status);
    run->started_at = r->started;
    session_set_tool_run(r->a->session, r->id, run, FALSE);
}

typedef struct { Agent *a; const char *id; ToolResult *res; double started; } RunFinish;

static void run_finish_main(gpointer p) {
    RunFinish *f = p;
    Session *s = f->a->session;
    ToolRun *old = session_tool_run(s, f->id);
    ToolRun *run = old ? tool_run_copy(old) : tool_run_new(TOOL_RUNNING);
    if (!old) run->started_at = f->started;
    if (run->status != TOOL_DENIED) run->status = f->res->is_error ? TOOL_ERROR : TOOL_DONE;
    run->finished_at = now_ts();
    g_free(run->output);
    run->output = utf8_prefix(f->res->text, 200000);
    g_clear_pointer(&run->progress, g_free);
    if (f->res->meta) {
        if (run->meta) json_node_unref(run->meta);
        run->meta = jnode_obj(f->res->meta);
    }
    session_set_tool_run(s, f->id, run, TRUE);
}


static void execute_call(ExecCtx *e) {
    Agent *a = e->a;
    Call *c = e->call;
    double started = now_ts();
    RunSet rs = { a, c->id, TOOL_RUNNING, started };
    main_sync(run_set_main, &rs);
    JsonNode *n = json_parse_str(c->input, -1);
    JsonObject *input = n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
    if (a->on_activity) {
        char *sum = tool_summary(c->name, input, NULL);
        char *line = *sum ? g_strdup_printf("%s · %s", tool_display_name(c->name), sum) : g_strdup(tool_display_name(c->name));
        a->on_activity(line, a->activity_data);
        g_free(line);
        g_free(sum);
    }
    ToolResult *res;
    const ToolDef *t = tool_find(a->tools, c->name);
    if (!t) res = tool_errf("Unknown tool: %s", c->name);
    else if (!input)
        res = tool_err(e->truncated ? "The tool input was truncated because the response hit the output token limit. Retry with smaller input "
                                      "(e.g. split a large write into several edits)."
                                    : "Invalid JSON tool input.");
    else {
        ToolCtx ctx = { a, c->id, a->session->cwd, a->step_model };
        res = t->run(input, &ctx);
    }
    if (n) json_node_unref(n);
    if (agent_cancelled(a) && !res->is_error) {
        char *t2 = g_strconcat(res->text, "\n[the user interrupted the turn after this call]", NULL);
        g_free(res->text);
        res->text = t2;
    }
    RunFinish f = { a, c->id, res, started };
    main_sync(run_finish_main, &f);
    GPtrArray *imgs = res->images;
    res->images = NULL;
    e->result = block_tool_result(c->id, res->text, res->is_error, imgs);
    tool_result_free(res);
}

static gpointer exec_thread(gpointer p) {
    execute_call(p);
    return NULL;
}

typedef struct { Agent *a; GPtrArray *calls; } PendingCtx;

static void set_pending_main(gpointer p) {
    PendingCtx *c = p;
    for (guint i = 0; i < c->calls->len; i++) {
        Call *call = g_ptr_array_index(c->calls, i);
        session_set_tool_run(c->a->session, call->id, tool_run_new(TOOL_PENDING), FALSE);
    }
}

typedef struct { Agent *a; const char *id; } CancelRun;

static void cancel_run_main(gpointer p) {
    CancelRun *c = p;
    ToolRun *r = tool_run_new(TOOL_CANCELLED);
    r->finished_at = now_ts();
    r->output = g_strdup("Cancelled");
    session_set_tool_run(c->a->session, c->id, r, TRUE);
}

static GPtrArray *run_tools(Agent *a, GPtrArray *calls, gboolean truncated) {
    PendingCtx pc = { a, calls };
    main_sync(set_pending_main, &pc);
    guint n = calls->len;
    ExecCtx *ctxs = g_new0(ExecCtx, n);
    for (guint i = 0; i < n; i++) {
        ctxs[i].a = a;
        ctxs[i].call = g_ptr_array_index(calls, i);
        ctxs[i].truncated = truncated;
    }
    guint i = 0;
    while (i < n) {
        if (agent_cancelled(a)) break;
        const ToolDef *t = tool_find(a->tools, ctxs[i].call->name);
        if (t && t->concurrency_safe) {
            guint j = i;
            while (j < n) {
                const ToolDef *tj = tool_find(a->tools, ctxs[j].call->name);
                if (!tj || !tj->concurrency_safe) break;
                j++;
            }
            if (j - i == 1) execute_call(&ctxs[i]);
            else {
                GThread **th = g_new(GThread *, j - i);
                for (guint k = i; k < j; k++) th[k - i] = g_thread_new("tool", exec_thread, &ctxs[k]);
                for (guint k = i; k < j; k++) g_thread_join(th[k - i]);
                g_free(th);
            }
            i = j;
        } else {
            execute_call(&ctxs[i]);
            i++;
        }
    }
    GPtrArray *out = g_ptr_array_new_with_free_func(block_free);
    for (guint k = 0; k < n; k++) {
        if (ctxs[k].result) { g_ptr_array_add(out, ctxs[k].result); continue; }
        CancelRun cr = { a, ctxs[k].call->id };
        main_sync(cancel_run_main, &cr);
        g_ptr_array_add(out, block_tool_result(ctxs[k].call->id, "The user interrupted this tool call before it ran.", TRUE, NULL));
    }
    g_free(ctxs);
    return out;
}

typedef struct { Agent *a; GPtrArray *steer; GPtrArray *notices; GPtrArray *images; } TakeCtx;

static void take_inputs_main(gpointer p) {
    TakeCtx *t = p;
    Session *s = t->a->session;
    t->steer = s->queued_inputs;
    s->queued_inputs = g_ptr_array_new_with_free_func(g_free);
    t->images = s->queued_images;
    s->queued_images = g_ptr_array_new_with_free_func(image_data_free);
    t->notices = t->a->pending_notices;
    t->a->pending_notices = g_ptr_array_new_with_free_func(g_free);
    if (t->steer->len) session_notify(s, CHANGE_META, NULL);
}

static void call_free(gpointer p) {
    Call *c = p;
    g_free(c->id);
    g_free(c->name);
    g_free(c->input);
    g_free(c);
}

/* ---------------- loop ---------------- */

TurnEnd agent_loop(Agent *a, char **error) {
    *error = NULL;
    BuildCtx b = { .a = a };
    main_sync(build_prompt_main, &b);
    TurnEnd result = END_COMPLETED;
    for (;;) {
        if (agent_cancelled(a)) { result = END_STOPPED; break; }
        if (a->depth == 0) compact_if_needed(a);
        if (agent_cancelled(a)) { result = END_STOPPED; break; }
        main_sync(build_body_main, &b);
        g_free(a->step_model);
        a->step_model = g_strdup(b.model);
        double started = now_ts();
        char *msg_id = uuid_new();
        DraftStart ds = { a, started, msg_id };
        main_sync(draft_start_main, &ds);
        dlog("step start (%zu bytes)", strlen(b.body));
        LLMError *err = NULL;
        Draft *d = stream_with_retry(a, b.body, &err);
        g_clear_pointer(&b.body, g_free);
        if (!d) {
            PartialCtx pc = { a, b.model, started, msg_id };
            main_sync(save_partial_main, &pc);
            g_free(msg_id);
            if (err->cancelled || agent_cancelled(a)) result = END_STOPPED;
            else { result = END_ERROR; *error = g_strdup(err->message); }
            llm_error_free(err);
            break;
        }
        double gen_start = d->first_token_at > 0 ? d->first_token_at : started;
        Message *m = message_new(ROLE_ASSISTANT);
        g_free(m->id);
        m->id = msg_id;
        GPtrArray *blocks = draft_blocks(d);
        for (guint i = 0; i < blocks->len; i++) message_add(m, block_copy(g_ptr_array_index(blocks, i)));
        m->has_usage = TRUE;
        m->usage = d->usage;
        m->model = g_strdup(b.model);
        m->duration = MAX(0.001, now_ts() - gen_start);
        GPtrArray *calls = g_ptr_array_new_with_free_func(call_free);
        for (guint i = 0; i < blocks->len; i++) {
            Block *bl = g_ptr_array_index(blocks, i);
            if (bl->type != BLK_TOOL_USE) continue;
            Call *c = g_new0(Call, 1);
            c->id = g_strdup(bl->id);
            c->name = g_strdup(bl->name);
            c->input = g_strdup(bl->input);
            g_ptr_array_add(calls, c);
        }
        g_ptr_array_unref(blocks);
        gboolean max_tokens = !g_strcmp0(d->stop_reason, "max_tokens");
        draft_free(d);
        AppendCtx ac = { a, m };
        main_sync(append_assistant_main, &ac);
        if (!calls->len) {
            g_ptr_array_unref(calls);
            if (max_tokens) { result = END_ERROR; *error = g_strdup("The response hit the output token limit."); }
            break;
        }
        GPtrArray *results = run_tools(a, calls, max_tokens);
        g_ptr_array_unref(calls);
        TakeCtx tc = { a };
        main_sync(take_inputs_main, &tc);
        Message *rm = message_new(ROLE_USER);
        rm->is_tool_results = TRUE;
        for (guint i = 0; i < results->len; i++) message_add(rm, block_copy(g_ptr_array_index(results, i)));
        g_ptr_array_unref(results);
        for (guint i = 0; i < tc.notices->len; i++) message_add(rm, block_text(g_ptr_array_index(tc.notices, i)));
        for (guint i = 0; i < tc.images->len; i++) message_add(rm, block_image(image_data_copy(g_ptr_array_index(tc.images, i))));
        for (guint i = 0; i < tc.steer->len; i++) message_add(rm, block_text(g_ptr_array_index(tc.steer, i)));
        rm->steered = tc.steer->len > 0;
        g_ptr_array_unref(tc.images);
        g_ptr_array_unref(tc.notices);
        g_ptr_array_unref(tc.steer);
        AppendCtx rc = { a, rm };
        main_sync(append_main, &rc);
        if (agent_cancelled(a)) { result = END_STOPPED; break; }
    }
    g_free(b.system);
    g_free(b.model);
    g_free(b.body);
    return result;
}

/* ---------------- approvals and questions ---------------- */

typedef struct {
    Agent *a;
    const char *call_id, *title, *detail, *key;
    gboolean preapproved;
    Waiter *w;
    char *req_id;
    int decision;
} ApprovalCtx;

static void approval_open_main(gpointer p) {
    ApprovalCtx *c = p;
    Session *s = c->a->session;
    if (g_hash_table_contains(s->session_approvals, c->key)) { c->preapproved = TRUE; return; }
    c->w = waiter_new();
    if (!s->running) { waiter_resolve(c->w, DECIDE_DENY, NULL); return; }
    ToolRun *r = session_tool_run(s, c->call_id);
    if (r) {
        ToolRun *x = tool_run_copy(r);
        x->status = TOOL_AWAITING;
        session_set_tool_run(s, c->call_id, x, FALSE);
    }
    ApprovalRequest *req = g_new0(ApprovalRequest, 1);
    req->id = uuid_new();
    req->call_id = g_strdup(c->call_id);
    req->title = g_strdup(c->title);
    req->detail = g_strdup(c->detail);
    waiter_ref(c->w);
    req->waiter = c->w;
    c->req_id = g_strdup(req->id);
    g_ptr_array_add(s->pending_approvals, req);
    session_notify(s, CHANGE_META, NULL);
    if (event_fn) event_fn(s, TRUE, event_data);
    store_emit(STORE_ATTENTION, s);
}

static void approval_close_main(gpointer p) {
    ApprovalCtx *c = p;
    Session *s = c->a->session;
    if (c->decision == DECIDE_SESSION) g_hash_table_add(s->session_approvals, g_strdup(c->key));
    for (guint i = 0; c->req_id && i < s->pending_approvals->len; i++) {
        ApprovalRequest *r = g_ptr_array_index(s->pending_approvals, i);
        if (!strcmp(r->id, c->req_id)) { g_ptr_array_remove_index(s->pending_approvals, i); break; }
    }
    ToolRun *r = session_tool_run(s, c->call_id);
    if (r) {
        ToolRun *x = tool_run_copy(r);
        x->status = c->decision == DECIDE_DENY ? TOOL_DENIED : TOOL_RUNNING;
        session_set_tool_run(s, c->call_id, x, FALSE);
    }
    session_notify(s, CHANGE_META, NULL);
}

gboolean agent_request_approval(Agent *a, const char *call_id, const char *title, const char *detail, const char *key) {
    if (a->parent) return agent_request_approval(a->parent, a->parent_call_id, title, detail, key);
    if (a->session->permission == PERM_FULL_ACCESS) return TRUE;
    if (agent_cancelled(a)) return FALSE;
    ApprovalCtx c = { .a = a, .call_id = call_id, .title = title, .detail = detail, .key = key };
    main_sync(approval_open_main, &c);
    if (c.preapproved) return TRUE;
    waiter_wait(c.w, &a->cancelled);
    /* Read the outcome under the lock: the UI may resolve between the wait timing out and the close. */
    g_mutex_lock(&c.w->m);
    c.decision = c.w->done ? c.w->value : DECIDE_DENY;
    g_mutex_unlock(&c.w->m);
    main_sync(approval_close_main, &c);
    waiter_unref(c.w);
    g_free(c.req_id);
    return c.decision != DECIDE_DENY;
}

void agent_resolve_approval(Agent *a, const char *id, ApprovalDecision d) {
    Session *s = a->session;
    for (guint i = 0; i < s->pending_approvals->len; i++) {
        ApprovalRequest *r = g_ptr_array_index(s->pending_approvals, i);
        if (!strcmp(r->id, id)) {
            waiter_resolve(r->waiter, d, NULL);
            g_ptr_array_remove_index(s->pending_approvals, i);
            session_notify(s, CHANGE_META, NULL);
            return;
        }
    }
}

typedef struct {
    Agent *a;
    const char *call_id;
    GPtrArray *questions;
    Waiter *w;
    char *req_id;
} AskCtx;

static void ask_open_main(gpointer p) {
    AskCtx *c = p;
    Session *s = c->a->session;
    c->w = waiter_new();
    if (!s->running) { waiter_resolve(c->w, 0, NULL); g_ptr_array_unref(c->questions); return; }
    QuestionRequest *q = g_new0(QuestionRequest, 1);
    q->id = uuid_new();
    q->call_id = g_strdup(c->call_id);
    q->questions = c->questions;
    waiter_ref(c->w);
    q->waiter = c->w;
    c->req_id = g_strdup(q->id);
    g_ptr_array_add(s->pending_questions, q);
    session_notify(s, CHANGE_META, NULL);
    if (event_fn) event_fn(s, TRUE, event_data);
    store_emit(STORE_ATTENTION, s);
}

static void ask_close_main(gpointer p) {
    AskCtx *c = p;
    Session *s = c->a->session;
    for (guint i = 0; c->req_id && i < s->pending_questions->len; i++) {
        QuestionRequest *q = g_ptr_array_index(s->pending_questions, i);
        if (!strcmp(q->id, c->req_id)) { g_ptr_array_remove_index(s->pending_questions, i); session_notify(s, CHANGE_META, NULL); break; }
    }
}

JsonObject *agent_ask_user(Agent *a, const char *call_id, GPtrArray *questions) {
    if (a->parent) return agent_ask_user(a->parent, a->parent_call_id, questions);
    AskCtx c = { a, call_id, questions };
    main_sync(ask_open_main, &c);
    waiter_wait(c.w, &a->cancelled);
    main_sync(ask_close_main, &c);
    /* After the request is closed nobody else can resolve it, so the payload is final. */
    g_mutex_lock(&c.w->m);
    JsonObject *answers = c.w->done ? c.w->payload : NULL;
    g_mutex_unlock(&c.w->m);
    waiter_unref(c.w);
    g_free(c.req_id);
    return answers;
}

void agent_resolve_question(Agent *a, const char *id, JsonObject *answers) {
    Session *s = a->session;
    for (guint i = 0; i < s->pending_questions->len; i++) {
        QuestionRequest *q = g_ptr_array_index(s->pending_questions, i);
        if (!strcmp(q->id, id)) {
            waiter_resolve(q->waiter, answers ? 1 : 0, answers);
            g_ptr_array_remove_index(s->pending_questions, i);
            session_notify(s, CHANGE_META, NULL);
            return;
        }
    }
    if (answers) json_object_unref(answers);
}

/* ---------------- tool context ---------------- */

char *ctx_resolve(ToolCtx *c, const char *path) {
    char *p = str_trim(path);
    if (!*p) { g_free(p); return g_strdup(c->cwd); }
    char *q = p;
    if (*q == '@') q++;
    size_t n = strlen(q);
    if (n >= 2 && q[0] == '"' && q[n - 1] == '"') { q[n - 1] = 0; q++; }
    char *e = path_expand_tilde(q);
    char *full = e[0] == '/' ? g_strdup(e) : g_build_filename(c->cwd, e, NULL);
    char *std = path_standardize(full);
    g_free(full);
    g_free(e);
    g_free(p);
    return std;
}

gboolean ctx_inside_workspace(ToolCtx *c, const char *path) {
    char *p = g_str_has_suffix(path, "/") ? g_strdup(path) : g_strconcat(path, "/", NULL);
    const char *roots[] = { c->cwd, g_get_tmp_dir(), "/tmp", "/var/tmp", NULL };
    gboolean inside = FALSE;
    for (int i = 0; roots[i] && !inside; i++) {
        char *r = path_standardize(roots[i]);
        char *rs = g_str_has_suffix(r, "/") ? g_strdup(r) : g_strconcat(r, "/", NULL);
        inside = g_str_has_prefix(p, rs);
        g_free(r);
        g_free(rs);
    }
    g_free(p);
    return inside;
}

typedef struct { Agent *a; char *id; char *text; JsonObject *meta; } CtxMsg;

static void progress_main(gpointer p) {
    CtxMsg *m = p;
    if (agent_alive(m->a)) session_update_tool_progress(m->a->session, m->id, m->text);
    g_free(m->id);
    g_free(m->text);
    g_free(m);
}

void ctx_progress(ToolCtx *c, const char *text) {
    CtxMsg *m = g_new0(CtxMsg, 1);
    m->a = c->agent;
    m->id = g_strdup(c->call_id);
    m->text = g_strdup(text);
    main_async(progress_main, m);
}

static void meta_main(gpointer p) {
    CtxMsg *m = p;
    if (agent_alive(m->a)) {
        ToolRun *r = session_tool_run(m->a->session, m->id);
        if (r) {
            ToolRun *x = tool_run_copy(r);
            if (x->meta) json_node_unref(x->meta);
            x->meta = jnode_obj(m->meta);
            session_set_tool_run(m->a->session, m->id, x, FALSE);
        }
    }
    json_object_unref(m->meta);
    g_free(m->id);
    g_free(m);
}

void ctx_set_meta(ToolCtx *c, JsonObject *meta) {
    CtxMsg *m = g_new0(CtxMsg, 1);
    m->a = c->agent;
    m->id = g_strdup(c->call_id);
    m->meta = meta;
    main_async(meta_main, m);
}

typedef struct { Session *s; const char *path; gboolean result; } ObsCtx;

static void observe_main(gpointer p) {
    ObsCtx *o = p;
    session_mark_observed(o->s, o->path);
}

static void has_observed_main(gpointer p) {
    ObsCtx *o = p;
    o->result = session_has_observed(o->s, o->path);
}

void ctx_observe(ToolCtx *c, const char *path) {
    ObsCtx o = { c->agent->session, path };
    main_sync(observe_main, &o);
}

gboolean ctx_has_observed(ToolCtx *c, const char *path) {
    ObsCtx o = { c->agent->session, path };
    main_sync(has_observed_main, &o);
    return o.result;
}

PermissionMode ctx_permission(ToolCtx *c) { return c->agent->session->permission; }

volatile gint *ctx_cancel(ToolCtx *c) { return &c->agent->cancelled; }
