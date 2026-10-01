#pragma once
#include "util.h"

typedef enum { BLK_TEXT, BLK_THINKING, BLK_TOOL_USE, BLK_TOOL_RESULT, BLK_IMAGE } BlockType;

typedef struct {
    char *media_type;
    char *base64;
    char *path;
} ImageData;

/* One content block of a Messages-API conversation turn. */
typedef struct {
    BlockType type;
    char *text;       /* text / thinking / tool_result content */
    char *signature;  /* thinking */
    char *id;         /* tool_use id, tool_result tool_use_id */
    char *name;       /* tool_use */
    char *input;      /* tool_use raw JSON */
    gboolean is_error;
    GPtrArray *images; /* ImageData*: tool_result images, or one image for BLK_IMAGE */
} Block;

typedef struct {
    gint64 input, output, cache_read, cache_write;
} Usage;

static inline gint64 usage_prompt(const Usage *u) { return u->input + u->cache_read + u->cache_write; }
static inline gint64 usage_total(const Usage *u) { return usage_prompt(u) + u->output; }
void usage_add(Usage *a, const Usage *b);

typedef enum { ROLE_USER, ROLE_ASSISTANT } Role;

typedef struct {
    char *id;
    Role role;
    GPtrArray *blocks; /* Block* */
    double time;
    gboolean has_usage;
    Usage usage;
    char *model;
    double duration;
    gboolean is_tool_results; /* user message that only carries tool results */
    gboolean steered;         /* text typed while the agent ran, spliced into the loop */
    gboolean notice;          /* harness notice (background job finished) */
} Message;

ImageData *image_data_new(const char *media_type, const char *base64, const char *path);
ImageData *image_data_copy(const ImageData *d);
void image_data_free(gpointer d);

Block *block_text(const char *t);
Block *block_thinking(const char *t, const char *sig);
Block *block_tool_use(const char *id, const char *name, const char *input);
Block *block_tool_result(const char *id, const char *content, gboolean is_error, GPtrArray *images /* taken */);
Block *block_image(ImageData *img /* taken */);
Block *block_copy(const Block *b);
void block_free(gpointer b);

Message *message_new(Role role);
Message *message_copy(const Message *m);
void message_free(gpointer m);
void message_add(Message *m, Block *b);
char *message_text(const Message *m); /* text blocks joined by blank lines */

/* Storage (session log) form. */
JsonObject *message_to_json(const Message *m);
Message *message_from_json(JsonObject *o);
/* Messages API wire form. */
JsonObject *message_wire(const Message *m);
JsonObject *block_wire(const Block *b);
