#pragma once
#include "conversation.h"
#include "settings.h"
#include "llm.h"
#include <stdio.h>

typedef struct Agent Agent;
typedef struct Session Session;

typedef enum { TOOL_PENDING, TOOL_RUNNING, TOOL_AWAITING, TOOL_DONE, TOOL_ERROR, TOOL_DENIED, TOOL_CANCELLED } ToolStatus;
const char *tool_status_raw(ToolStatus s);
ToolStatus tool_status_parse(const char *s);

/* Runtime state of one tool call, keyed by call id. */
typedef struct {
    ToolStatus status;
    double started_at, finished_at;
    char *output;
    char *progress;
    JsonNode *meta; /* object or NULL */
} ToolRun;
ToolRun *tool_run_new(ToolStatus st);
ToolRun *tool_run_copy(const ToolRun *r);
void tool_run_free(gpointer r);
gint64 tool_meta_int(const ToolRun *r, const char *k, gint64 def);
gboolean tool_meta_has(const ToolRun *r, const char *k);
const char *tool_meta_str(const ToolRun *r, const char *k);

typedef enum { END_NONE, END_COMPLETED, END_STOPPED, END_ERROR } TurnEnd;

typedef struct {
    int index;
    double started_at, ended_at;
    TurnEnd end;
    char *error;
    int message_index; /* index of the human message that started the turn */
} TurnRecord;

typedef struct {
    char *content;
    char *status; /* pending | in_progress | completed */
} TodoItem;
void todo_free(gpointer t);

typedef enum { DECIDE_DENY = 0, DECIDE_ONCE = 1, DECIDE_SESSION = 2 } ApprovalDecision;

typedef struct {
    char *id;
    char *call_id;
    char *title;
    char *detail;
    Waiter *waiter;
} ApprovalRequest;

typedef struct {
    char *id;
    char *header;
    char *question;
    GPtrArray *labels; /* char* */
    GPtrArray *descs;  /* char* (may be "") */
    gboolean multi;
} Question;
void question_free(gpointer q);

typedef struct {
    char *id;
    char *call_id;
    GPtrArray *questions; /* Question* */
    Waiter *waiter;       /* payload: JsonObject* answers (id -> [labels]) or NULL */
} QuestionRequest;

typedef enum { CHANGE_RESET, CHANGE_DRAFT, CHANGE_TOOL, CHANGE_META } ChangeKind;
typedef void (*SessionObserver)(Session *s, ChangeKind kind, const char *tool_id, gpointer data);

struct Session {
    char *id;
    char *cwd;
    char *title;
    gboolean has_title;
    double created_at, updated_at;

    GPtrArray *messages;   /* Message* */
    GHashTable *tool_runs; /* call id -> ToolRun* */
    GPtrArray *turns;      /* TurnRecord* */
    GPtrArray *todos;      /* TodoItem* */
    GHashTable *observed;  /* path set */
    int compaction_index;  /* -1 when none */
    char *compaction_summary;
    char *model;
    Effort effort;
    PermissionMode permission;

    /* runtime */
    Draft *draft;
    char *draft_id;
    double draft_started_at;
    gboolean running;
    char *status_line;
    GPtrArray *pending_approvals; /* ApprovalRequest* */
    GPtrArray *pending_questions; /* QuestionRequest* */
    GPtrArray *queued_inputs;     /* char* */
    GHashTable *session_approvals;
    char *last_error;
    Agent *agent;
    gboolean ephemeral;
    gboolean loaded;
    gboolean deleted; /* removed from the index; never starts another turn */
    GPtrArray *queued_images; /* ImageData* attached to queued inputs */

    char *file_path;
    FILE *fh;
    GPtrArray *observers;
    int observer_freeze;
};

Session *session_new(const char *cwd);
Session *session_open(const char *id, const char *cwd, const char *title, double updated_at, double created_at);
void session_free(Session *s);
void session_load(Session *s);
void session_unload(Session *s);
void session_delete_file(Session *s);

guint session_observe(Session *s, SessionObserver fn, gpointer data);
void session_unobserve(Session *s, gpointer data);
void session_notify(Session *s, ChangeKind kind, const char *tool_id);

/* Mutations (main thread). */
void session_append(Session *s, Message *m /* taken */);
void session_set_tool_run(Session *s, const char *id, ToolRun *r /* taken */, gboolean persist);
void session_update_tool_progress(Session *s, const char *id, const char *progress);
TurnRecord *session_begin_turn(Session *s, int message_index);
void session_end_turn(Session *s, int index, TurnEnd end, const char *error);
void session_set_title(Session *s, const char *t);
void session_set_todos(Session *s, GPtrArray *todos /* taken */);
void session_mark_observed(Session *s, const char *path);
gboolean session_has_observed(Session *s, const char *path);
void session_set_compaction(Session *s, int index, const char *summary);
void session_save_config(Session *s);
void session_set_draft(Session *s, Draft *d /* taken, NULL clears */);
void session_repair_dangling(Session *s, gboolean persist);

/* Wire messages JSON array (compaction applied, unreplayable thinking removed). */
JsonArray *session_wire_messages(Session *s, const char *model);

/* Derived */
Usage session_total_usage(Session *s);
gint64 session_last_prompt_tokens(Session *s);
int session_step_count(Session *s);
double session_output_rate(Session *s);  /* < 0 when unknown */
double session_cache_hit_rate(Session *s); /* < 0 when unknown */
ToolRun *session_tool_run(Session *s, const char *id);
TurnRecord *session_last_turn(Session *s);
char *session_export_markdown(Session *s);
const Message *session_last_assistant_with_text(Session *s);
