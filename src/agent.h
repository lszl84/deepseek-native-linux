#pragma once
#include "session.h"
#include "jobs.h"

typedef struct ToolCtx ToolCtx;

typedef struct {
    char *text;
    gboolean is_error;
    GPtrArray *images; /* ImageData* */
    JsonObject *meta;
} ToolResult;

ToolResult *tool_ok(const char *text);
ToolResult *tool_err(const char *text);
ToolResult *tool_errf(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void tool_result_free(ToolResult *r);

typedef struct {
    const char *name;
    const char *description;
    const char *schema;
    gboolean concurrency_safe;
    ToolResult *(*run)(JsonObject *input, ToolCtx *ctx);
} ToolDef;

/* Tools available at a nesting depth, sorted by name. Container only; defs are static. */
GPtrArray *tool_registry(int depth);
const ToolDef *tool_find(GPtrArray *tools, const char *name);

struct Agent {
    Session *session;
    JobManager *jobs;
    int depth;
    GPtrArray *tools;
    GThread *thread;
    volatile gint cancelled;
    Agent *parent;
    char *parent_call_id;
    void (*on_activity)(const char *line, gpointer data);
    gpointer activity_data;
    GMutex lock;
    Draft *pending_draft;
    gboolean flush_scheduled;
    GPtrArray *pending_notices; /* char*, main thread */
    GPtrArray *children;        /* Agent*, under lock */
    int turn_index;
    char *step_model; /* model of the current step; owned by the loop thread, read by its tools */
};

struct ToolCtx {
    Agent *agent;
    char *call_id;
    char *cwd;
    const char *model; /* snapshot; do not read agent->session->model from tool threads */
};

Agent *agent_new(Session *s, int depth);
void agent_free(Agent *a);
Agent *agent_for(Session *s); /* existing or new */
gboolean agent_has_running_jobs(Agent *a);

/* Main thread. */
void agent_submit(Agent *a, const char *text, GPtrArray *images /* ImageData*, taken; may be NULL */);
void agent_cancel(Agent *a);
void agent_resolve_approval(Agent *a, const char *id, ApprovalDecision d);
void agent_resolve_question(Agent *a, const char *id, JsonObject *answers /* taken, NULL = dismissed */);

/* Worker side. */
TurnEnd agent_loop(Agent *a, char **error);
gboolean agent_cancelled(Agent *a);
gboolean agent_request_approval(Agent *a, const char *call_id, const char *title, const char *detail, const char *key);
JsonObject *agent_ask_user(Agent *a, const char *call_id, GPtrArray *questions /* Question*, taken */);

/* Tool context helpers (worker thread). */
char *ctx_resolve(ToolCtx *c, const char *path);
gboolean ctx_inside_workspace(ToolCtx *c, const char *path);
void ctx_progress(ToolCtx *c, const char *text);
void ctx_set_meta(ToolCtx *c, JsonObject *meta /* taken */);
void ctx_observe(ToolCtx *c, const char *path);
gboolean ctx_has_observed(ToolCtx *c, const char *path);
PermissionMode ctx_permission(ToolCtx *c);
volatile gint *ctx_cancel(ToolCtx *c);

/* Global agent events for the UI (main thread). */
typedef void (*AgentEventFn)(Session *s, gboolean attention, gpointer data);
void agent_set_event_handler(AgentEventFn fn, gpointer data);

char *system_prompt_build(Session *s, GPtrArray *tools, int depth);
