#pragma once
#include "markdown.h"

typedef enum {
    HIT_NONE, HIT_TOGGLE, HIT_COPY, HIT_LINK, HIT_APPROVE, HIT_CHOOSE, HIT_SUBMIT, HIT_DISMISS, HIT_OPEN_FILE, HIT_RETRY, HIT_CANCEL_QUEUED
} HitKind;

typedef struct {
    HitKind kind;
    char *a, *b, *c; /* toggle key / copy text / url / request id, question id, label / path */
    int n;           /* approval decision / queued index */
} Hit;

typedef struct {
    Hit hit;
    double x, y, w, h; /* local rect for hover feedback */
    char *key;
} HitResult;

Hit hit_make(HitKind k, const char *a, const char *b, const char *c, int n);
Hit hit_copy(const Hit *h);
void hit_free_contents(Hit *h);
void hit_result_clear(HitResult *r);
void hit_result_offset(HitResult *r, double dx, double dy);

typedef struct {
    char *hover_key;
    char *hover_item;
    GHashTable *selection; /* TextLayout* -> gint64 packed (start << 32 | end) */
    double phase;
    char *copied_key;
    char *pressed_key;
    double clip_top, clip_bottom;
    int scale;
} DrawEnv;

gboolean env_selection(DrawEnv *env, TextLayout *tl, int *s, int *e);

typedef struct {
    TextLayout *tl;
    double x, y;
} TextRef;

typedef struct El El;
typedef struct {
    void (*layout)(El *, double w);
    void (*draw)(El *, cairo_t *, double x, double y, DrawEnv *);
    gboolean (*hit)(El *, double x, double y, HitResult *);
    void (*texts)(El *, double x, double y, GArray *refs);
    gboolean (*animating)(El *);
    void (*destroy)(El *);
} ElClass;

struct El {
    const ElClass *k;
    double w, h;
};

void el_layout(El *e, double w);
void el_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env);
gboolean el_hit(El *e, double x, double y, HitResult *r);
void el_texts(El *e, double x, double y, GArray *refs);
gboolean el_animating(El *e);
void el_free(El *e);

typedef enum { TR_NONE, TR_SPINNER, TR_TEXT, TR_DIFF, TR_ICON } TrailingKind;
typedef struct {
    TrailingKind kind;
    char *text;
    ColorId color;
    int added, removed;
} Trailing;

typedef enum { BTN_PRIMARY, BTN_SECONDARY, BTN_PLAIN, BTN_DANGER } ButtonStyle;

El *el_text(RichText *rt, int spacing);
El *el_text_max(RichText *rt, int spacing, int max_lines);
El *el_text_insets(RichText *rt, int spacing, double l, double t, double r, double b);
El *el_mono(const char *s, ColorId c, FontId f);
El *el_spacer(double h);
El *el_rule(double margin);
El *el_vstack(GPtrArray *children /* El*, taken */, double spacing);
El *el_box(El *child, double pt, double pl, double pb, double pr, int bg /* ColorId or -1 */, int border, double radius);
El *el_detail_box(GPtrArray *children);
El *el_code(const char *lang, const char *code, const char *key);
El *el_table(GPtrArray *header /* RichText* taken */, GPtrArray *rows /* GPtrArray<RichText*> taken */, const int *aligns, int ncols);
El *el_list(GPtrArray *markers /* char*, taken */, GArray *tasks /* int, taken */, GPtrArray *contents /* El*, taken */, FontId f, ColorId c);
El *el_quote(El *child);
El *el_image(const char *path, const char *alt);
El *el_user_bubble(const char *text, const char *time, const char *key, gboolean faded, gboolean notice);
El *el_turn_header(const char *label, ColorId color, gboolean spinning, double live_since);
El *el_row(const char *icon, const char *title, const char *subtitle, Trailing tr, const char *key, Hit hit, int expanded,
           ColorId title_color, FontId title_font, double height);
El *el_button(const char *title, const char *icon, ButtonStyle style, Hit hit, const char *key);
El *el_button_row(GPtrArray *buttons /* El* taken */);
El *el_footer(const char *copy_text, const char *usage, const char *time, const char *key);
El *el_diff(GPtrArray *lines /* char* taken */, GArray *kinds /* int: -1, 0, 1, taken */);
El *el_indent(El *child, double left);
El *el_option(const char *label, const char *detail, gboolean selected, gboolean multi, Hit hit, const char *key);
El *el_file_card(const char *path, const char *detail, const char *key);

/* Markdown blocks → elements (takes ownership of the blocks array). */
El *md_render(GPtrArray *blocks, const MDStyle *st, const char *key_prefix);
El *md_element(const char *text, const MDStyle *st, const char *key_prefix);
