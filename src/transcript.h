#pragma once
#include "builder.h"

#define DS_TYPE_TRANSCRIPT (ds_transcript_get_type())
G_DECLARE_FINAL_TYPE(DsTranscript, ds_transcript, DS, TRANSCRIPT, GtkDrawingArea)

typedef void (*TranscriptHitFn)(const Hit *hit, gpointer data);
typedef void (*TranscriptTypeFn)(GdkEventKey *ev, gpointer data);

GtkWidget *ds_transcript_new(void);
/* Applies new item specs, reusing items whose key and signature are unchanged. Takes the array. */
void ds_transcript_apply(DsTranscript *t, GPtrArray *specs);
gboolean ds_transcript_have(const char *key, Sig sig, gpointer self);
void ds_transcript_clear(DsTranscript *t);
void ds_transcript_set_hit_handler(DsTranscript *t, TranscriptHitFn fn, gpointer data);
void ds_transcript_set_type_handler(DsTranscript *t, TranscriptTypeFn fn, gpointer data);
void ds_transcript_set_insets(DsTranscript *t, double top, double bottom, double scrim);
void ds_transcript_invalidate_item(DsTranscript *t, const char *key);
void ds_transcript_relayout_all(DsTranscript *t);
void ds_transcript_clear_selection(DsTranscript *t);
char *ds_transcript_selected_text(DsTranscript *t);
gboolean ds_transcript_at_bottom(DsTranscript *t);
void ds_transcript_scroll_to_bottom(DsTranscript *t);
void ds_transcript_scroll_to(DsTranscript *t, double y);
double ds_transcript_content_height(DsTranscript *t);
void ds_transcript_set_scroll_handler(DsTranscript *t, void (*fn)(gpointer), gpointer data);
gboolean ds_transcript_find_hit(DsTranscript *t, const char *needle, double *wx, double *wy);
gboolean ds_transcript_render_document(DsTranscript *t, const char *path);
GPtrArray *ds_transcript_keys(DsTranscript *t); /* borrowed key strings */
