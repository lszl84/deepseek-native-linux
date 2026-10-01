#pragma once
#include "conversation.h"
#include "settings.h"

/* Assistant message assembled from stream events. */
typedef struct {
    BlockType type; /* BLK_TEXT, BLK_THINKING or BLK_TOOL_USE */
    GString *text;  /* text / thinking / tool input JSON */
    GString *sig;
    char *id;
    char *name;
} DraftBlock;

typedef struct {
    GPtrArray *blocks; /* DraftBlock* */
    Usage usage;
    char *stop_reason;
    double first_token_at;
} Draft;

Draft *draft_new(void);
Draft *draft_copy(const Draft *d);
void draft_free(gpointer d);
/* Converts to content blocks (tool inputs default to "{}"). */
GPtrArray *draft_blocks(const Draft *d);

typedef struct {
    char *message;
    int status;
    gboolean retryable;
    double retry_after;
    gboolean cancelled;
} LLMError;
void llm_error_free(LLMError *e);

typedef void (*DraftCallback)(const Draft *d, gpointer data);

/* Streams one Messages request (body is the JSON request). Blocks the calling thread. */
Draft *llm_stream(const char *body, const char *session_id, volatile gint *cancel,
                  DraftCallback on_event, gpointer data, LLMError **err);
/* Non-streaming JSON request. Returns the parsed response object node. */
JsonNode *llm_complete(const char *body, const char *session_id, volatile gint *cancel, LLMError **err);
/* Short session title with thinking disabled; NULL on failure. */
char *llm_title(const char *text, const char *model);
/* Concatenated text blocks of a non-streaming response. */
char *llm_response_text(JsonNode *resp);

void llm_global_init(void);
