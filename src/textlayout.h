#pragma once
#include "theme.h"

/* Attributed UTF-8 text with theme-resolved spans. */
typedef struct {
    int start, end;
    FontId font;
    int flags;   /* FS_* */
    ColorId color;
    double alpha;
    int link;    /* index into links, -1 when none */
} RSpan;

typedef struct {
    GString *text;
    GArray *spans;     /* RSpan */
    GPtrArray *links;  /* char* */
} RichText;

RichText *rt_new(void);
void rt_free(RichText *rt);
void rt_append(RichText *rt, const char *s, gssize len, FontId f, int flags, ColorId c);
void rt_append_link(RichText *rt, const char *s, gssize len, FontId f, int flags, ColorId c, const char *url);
void rt_append_alpha(RichText *rt, const char *s, gssize len, FontId f, int flags, ColorId c, double alpha);
RichText *rt_plain(const char *s, FontId f, int flags, ColorId c);

typedef struct {
    RichText *rt;
    PangoLayout *layout;
    int spacing;            /* px between lines */
    PangoAlignment align;
    gboolean char_wrap;
    int max_lines;
    double laid_w;
    guint gen;
    double w, h;
    gboolean has_code;
} TextLayout;

TextLayout *tl_new(RichText *rt /* taken */, int spacing);
void tl_free(TextLayout *t);
void tl_layout(TextLayout *t, double width);
void tl_draw(TextLayout *t, cairo_t *cr, double x, double y, double clip_top, double clip_bottom, int sel_start, int sel_end);
int tl_length(TextLayout *t);
const char *tl_text(TextLayout *t);
int tl_index_at(TextLayout *t, double x, double y);
const char *tl_link_at(TextLayout *t, double x, double y);
gboolean tl_contains(TextLayout *t, double x, double y);
void tl_word_range(TextLayout *t, int idx, int *s, int *e);
void tl_line_range(TextLayout *t, int idx, int *s, int *e);
double tl_longest_word(TextLayout *t);
