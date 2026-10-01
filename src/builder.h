#pragma once
#include "elements.h"
#include "session.h"

typedef struct {
    char *key;
    Sig sig;
    double gap;
    El *el; /* NULL: reuse the existing item with this key */
} ItemSpec;

void item_spec_free(gpointer p);

/* UI state of the transcript that survives rebuilds. */
typedef struct {
    GHashTable *expanded;
    GHashTable *collapsed;
    GHashTable *selected; /* "request/question" -> GHashTable set of labels */
    GHashTable *effective_expanded;
} TranscriptState;

TranscriptState *tstate_new(void);
void tstate_free(TranscriptState *s);
void tstate_reset(TranscriptState *s);
gboolean tstate_option_selected(TranscriptState *s, const char *req, const char *q, const char *label);
void tstate_toggle_option(TranscriptState *s, const char *req, const char *q, const char *label, gboolean multi);
GPtrArray *tstate_options(TranscriptState *s, const char *req, const char *q); /* char*, caller frees container */

typedef gboolean (*HaveItemFn)(const char *key, Sig sig, gpointer data);

GPtrArray *transcript_build(Session *s, TranscriptState *st, HaveItemFn have, gpointer data);
