#include "elements.h"
#include "util.h"
#include <math.h>
#include <string.h>

/* ======================= hits ======================= */

Hit hit_make(HitKind k, const char *a, const char *b, const char *c, int n) {
    return (Hit){ k, g_strdup(a), g_strdup(b), g_strdup(c), n };
}

Hit hit_copy(const Hit *h) { return hit_make(h->kind, h->a, h->b, h->c, h->n); }

void hit_free_contents(Hit *h) {
    g_free(h->a);
    g_free(h->b);
    g_free(h->c);
    memset(h, 0, sizeof *h);
}

void hit_result_clear(HitResult *r) {
    hit_free_contents(&r->hit);
    g_free(r->key);
    memset(r, 0, sizeof *r);
}

void hit_result_offset(HitResult *r, double dx, double dy) {
    r->x += dx;
    r->y += dy;
}

static gboolean set_hit(HitResult *r, const Hit *h, double x, double y, double w, double hh, const char *key) {
    r->hit = hit_copy(h);
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = hh;
    r->key = g_strdup(key);
    return TRUE;
}

gboolean env_selection(DrawEnv *env, TextLayout *tl, int *s, int *e) {
    if (!env->selection) return FALSE;
    gpointer v;
    if (!g_hash_table_lookup_extended(env->selection, tl, NULL, &v)) return FALSE;
    gint64 packed = (gint64)(gintptr)v;
    *s = (int)(packed >> 32);
    *e = (int)(packed & 0xffffffff);
    return TRUE;
}

static void draw_tl(TextLayout *tl, cairo_t *cr, double x, double y, DrawEnv *env) {
    int s = -1, e = -1;
    env_selection(env, tl, &s, &e);
    tl_draw(tl, cr, x, y, env->clip_top, env->clip_bottom, s, e);
}

static void add_ref(GArray *refs, TextLayout *tl, double x, double y) {
    TextRef r = { tl, x, y };
    g_array_append_val(refs, r);
}

/* ======================= generic dispatch ======================= */

void el_layout(El *e, double w) { if (e->k->layout) e->k->layout(e, w); else { e->w = w; e->h = 0; } }
void el_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    if (!e->k->draw) return;
    if (y > env->clip_bottom || y + e->h < env->clip_top) return;
    e->k->draw(e, cr, x, y, env);
}
gboolean el_hit(El *e, double x, double y, HitResult *r) { return e->k->hit ? e->k->hit(e, x, y, r) : FALSE; }
void el_texts(El *e, double x, double y, GArray *refs) { if (e->k->texts) e->k->texts(e, x, y, refs); }
gboolean el_animating(El *e) { return e->k->animating ? e->k->animating(e) : FALSE; }
void el_free(El *e) {
    if (!e) return;
    if (e->k->destroy) e->k->destroy(e);
    g_free(e);
}

static void el_free_cb(gpointer p) { el_free(p); }

/* ======================= text ======================= */

typedef struct {
    El base;
    TextLayout *tl;
    double il, it, ir, ib;
} TextEl;

static void text_layout(El *e, double w) {
    TextEl *t = (TextEl *)e;
    tl_layout(t->tl, MAX(10, w - t->il - t->ir));
    e->w = w;
    e->h = t->tl->h + t->it + t->ib;
}

static void text_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    TextEl *t = (TextEl *)e;
    draw_tl(t->tl, cr, x + t->il, y + t->it, env);
}

static gboolean text_hit(El *e, double x, double y, HitResult *r) {
    TextEl *t = (TextEl *)e;
    const char *link = tl_link_at(t->tl, x - t->il, y - t->it);
    if (!link) return FALSE;
    Hit h = hit_make(HIT_LINK, link, NULL, NULL, 0);
    char *key = g_strconcat("link:", link, NULL);
    set_hit(r, &h, 0, 0, 0, 0, key);
    g_free(key);
    hit_free_contents(&h);
    return TRUE;
}

static void text_texts(El *e, double x, double y, GArray *refs) {
    TextEl *t = (TextEl *)e;
    add_ref(refs, t->tl, x + t->il, y + t->it);
}

static void text_destroy(El *e) { tl_free(((TextEl *)e)->tl); }

static const ElClass TEXT_CLASS = { text_layout, text_draw, text_hit, text_texts, NULL, text_destroy };

El *el_text_insets(RichText *rt, int spacing, double l, double t, double r, double b) {
    TextEl *e = g_new0(TextEl, 1);
    e->base.k = &TEXT_CLASS;
    e->tl = tl_new(rt, spacing);
    e->il = l;
    e->it = t;
    e->ir = r;
    e->ib = b;
    return &e->base;
}

El *el_text(RichText *rt, int spacing) { return el_text_insets(rt, spacing, 0, 0, 0, 0); }

El *el_text_max(RichText *rt, int spacing, int max_lines) {
    El *e = el_text(rt, spacing);
    ((TextEl *)e)->tl->max_lines = max_lines;
    return e;
}

El *el_mono(const char *s, ColorId c, FontId f) {
    El *e = el_text(rt_plain(s, f, 0, c), 3);
    ((TextEl *)e)->tl->char_wrap = TRUE;
    return e;
}

/* ======================= spacer / rule ======================= */

typedef struct { El base; double v; } SimpleEl;

static void spacer_layout(El *e, double w) { e->w = w; e->h = ((SimpleEl *)e)->v; }
static const ElClass SPACER_CLASS = { spacer_layout, NULL, NULL, NULL, NULL, NULL };

El *el_spacer(double h) {
    SimpleEl *e = g_new0(SimpleEl, 1);
    e->base.k = &SPACER_CLASS;
    e->v = h;
    return &e->base;
}

static void rule_layout(El *e, double w) { e->w = w; e->h = ((SimpleEl *)e)->v * 2 + 1; }
static void rule_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    set_color(cr, C_SEPARATOR);
    cairo_rectangle(cr, x, y + ((SimpleEl *)e)->v, e->w, 1);
    cairo_fill(cr);
}
static const ElClass RULE_CLASS = { rule_layout, rule_draw, NULL, NULL, NULL, NULL };

El *el_rule(double margin) {
    SimpleEl *e = g_new0(SimpleEl, 1);
    e->base.k = &RULE_CLASS;
    e->v = margin;
    return &e->base;
}

/* ======================= vstack ======================= */

typedef struct {
    El base;
    GPtrArray *children;
    double spacing;
    double *offsets;
} StackEl;

static void stack_layout(El *e, double w) {
    StackEl *s = (StackEl *)e;
    double y = 0;
    g_free(s->offsets);
    s->offsets = g_new(double, MAX(1, s->children->len));
    for (guint i = 0; i < s->children->len; i++) {
        if (i > 0) y += s->spacing;
        s->offsets[i] = y;
        El *c = g_ptr_array_index(s->children, i);
        el_layout(c, w);
        y += c->h;
    }
    e->w = w;
    e->h = y;
}

static void stack_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    StackEl *s = (StackEl *)e;
    for (guint i = 0; i < s->children->len; i++) {
        El *c = g_ptr_array_index(s->children, i);
        double cy = y + s->offsets[i];
        if (cy > env->clip_bottom) break;
        if (cy + c->h < env->clip_top) continue;
        el_draw(c, cr, x, cy, env);
    }
}

static gboolean stack_hit(El *e, double x, double y, HitResult *r) {
    StackEl *s = (StackEl *)e;
    for (guint i = 0; i < s->children->len; i++) {
        El *c = g_ptr_array_index(s->children, i);
        double cy = s->offsets[i];
        if (y >= cy && y < cy + c->h) {
            if (el_hit(c, x, y - cy, r)) { hit_result_offset(r, 0, cy); return TRUE; }
            return FALSE;
        }
    }
    return FALSE;
}

static void stack_texts(El *e, double x, double y, GArray *refs) {
    StackEl *s = (StackEl *)e;
    for (guint i = 0; i < s->children->len; i++) el_texts(g_ptr_array_index(s->children, i), x, y + s->offsets[i], refs);
}

static gboolean stack_anim(El *e) {
    StackEl *s = (StackEl *)e;
    for (guint i = 0; i < s->children->len; i++)
        if (el_animating(g_ptr_array_index(s->children, i))) return TRUE;
    return FALSE;
}

static void stack_destroy(El *e) {
    StackEl *s = (StackEl *)e;
    g_ptr_array_unref(s->children);
    g_free(s->offsets);
}

static const ElClass STACK_CLASS = { stack_layout, stack_draw, stack_hit, stack_texts, stack_anim, stack_destroy };

El *el_vstack(GPtrArray *children, double spacing) {
    StackEl *s = g_new0(StackEl, 1);
    s->base.k = &STACK_CLASS;
    g_ptr_array_set_free_func(children, el_free_cb);
    s->children = children;
    s->spacing = spacing;
    return &s->base;
}

/* ======================= box ======================= */

typedef struct {
    El base;
    El *child;
    double pt, pl, pb, pr;
    int bg, border;
    double radius;
} BoxEl;

static void box_layout(El *e, double w) {
    BoxEl *b = (BoxEl *)e;
    el_layout(b->child, w - b->pl - b->pr);
    e->w = w;
    e->h = b->child->h + b->pt + b->pb;
}

static void box_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    BoxEl *b = (BoxEl *)e;
    if (b->bg >= 0) fill_rounded(cr, x, y, e->w, e->h, b->radius, th(b->bg));
    if (b->border >= 0) stroke_rounded(cr, x, y, e->w, e->h, b->radius, th(b->border), 1);
    el_draw(b->child, cr, x + b->pl, y + b->pt, env);
}

static gboolean box_hit(El *e, double x, double y, HitResult *r) {
    BoxEl *b = (BoxEl *)e;
    if (!el_hit(b->child, x - b->pl, y - b->pt, r)) return FALSE;
    hit_result_offset(r, b->pl, b->pt);
    return TRUE;
}

static void box_texts(El *e, double x, double y, GArray *refs) {
    BoxEl *b = (BoxEl *)e;
    el_texts(b->child, x + b->pl, y + b->pt, refs);
}

static gboolean box_anim(El *e) { return el_animating(((BoxEl *)e)->child); }
static void box_destroy(El *e) { el_free(((BoxEl *)e)->child); }

static const ElClass BOX_CLASS = { box_layout, box_draw, box_hit, box_texts, box_anim, box_destroy };

El *el_box(El *child, double pt, double pl, double pb, double pr, int bg, int border, double radius) {
    BoxEl *b = g_new0(BoxEl, 1);
    b->base.k = &BOX_CLASS;
    b->child = child;
    b->pt = pt;
    b->pl = pl;
    b->pb = pb;
    b->pr = pr;
    b->bg = bg;
    b->border = border;
    b->radius = radius;
    return &b->base;
}

El *el_detail_box(GPtrArray *children) { return el_box(el_vstack(children, 8), 10, 12, 10, 12, C_CODE_BG, C_CODE_BORDER, 10); }

/* ======================= indent ======================= */

typedef struct { El base; El *child; double left; } IndentEl;

static void indent_layout(El *e, double w) {
    IndentEl *i = (IndentEl *)e;
    el_layout(i->child, w - i->left);
    e->w = w;
    e->h = i->child->h;
}
static void indent_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) { el_draw(((IndentEl *)e)->child, cr, x + ((IndentEl *)e)->left, y, env); }
static gboolean indent_hit(El *e, double x, double y, HitResult *r) {
    IndentEl *i = (IndentEl *)e;
    if (!el_hit(i->child, x - i->left, y, r)) return FALSE;
    hit_result_offset(r, i->left, 0);
    return TRUE;
}
static void indent_texts(El *e, double x, double y, GArray *refs) { el_texts(((IndentEl *)e)->child, x + ((IndentEl *)e)->left, y, refs); }
static gboolean indent_anim(El *e) { return el_animating(((IndentEl *)e)->child); }
static void indent_destroy(El *e) { el_free(((IndentEl *)e)->child); }
static const ElClass INDENT_CLASS = { indent_layout, indent_draw, indent_hit, indent_texts, indent_anim, indent_destroy };

El *el_indent(El *child, double left) {
    IndentEl *i = g_new0(IndentEl, 1);
    i->base.k = &INDENT_CLASS;
    i->child = child;
    i->left = left;
    return &i->base;
}

/* ======================= code block ======================= */

typedef struct {
    El base;
    char *lang, *code, *key;
    TextLayout *body;
} CodeEl;

#define CODE_HEADER 32
#define CODE_PAD 14

static void code_layout(El *e, double w) {
    CodeEl *c = (CodeEl *)e;
    tl_layout(c->body, w - CODE_PAD * 2);
    e->w = w;
    e->h = CODE_HEADER + c->body->h + CODE_PAD + 2;
}

static void code_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    CodeEl *c = (CodeEl *)e;
    fill_rounded(cr, x, y, e->w, e->h, 12, th(C_CODE_BG));
    stroke_rounded(cr, x, y, e->w, e->h, 12, th(C_CODE_BORDER), 1);
    draw_label(cr, *c->lang ? c->lang : "text", x + CODE_PAD, y, 200, CODE_HEADER, F_CAPTION, 0, th(C_TEXT3), PANGO_ALIGN_LEFT);
    double cx = x + e->w - 72, cy = y + 4;
    gboolean copied = !g_strcmp0(env->copied_key, c->key);
    if (!g_strcmp0(env->hover_key, c->key)) fill_rounded(cr, cx, cy, 66, 24, 6, th(C_HOVER));
    if (copied) draw_check(cr, cx + 6, cy + 5, 14, 14, th(C_TEXT3), 1.5);
    else draw_icon(cr, "edit-copy-symbolic", cx + 4, cy, 18, 24, 12, th(C_TEXT3));
    draw_label(cr, copied ? "Copied" : "Copy", cx + 24, cy, 44, 24, F_CAPTION, 0, th(C_TEXT3), PANGO_ALIGN_LEFT);
    draw_tl(c->body, cr, x + CODE_PAD, y + CODE_HEADER, env);
}

static gboolean code_hit(El *e, double x, double y, HitResult *r) {
    CodeEl *c = (CodeEl *)e;
    double cx = e->w - 72, cy = 4;
    if (x >= cx && x <= cx + 66 && y >= cy && y <= cy + 24) {
        Hit h = hit_make(HIT_COPY, c->code, NULL, NULL, 0);
        set_hit(r, &h, cx, cy, 66, 24, c->key);
        hit_free_contents(&h);
        return TRUE;
    }
    return FALSE;
}

static void code_texts(El *e, double x, double y, GArray *refs) { add_ref(refs, ((CodeEl *)e)->body, x + CODE_PAD, y + CODE_HEADER); }

static void code_destroy(El *e) {
    CodeEl *c = (CodeEl *)e;
    g_free(c->lang);
    g_free(c->code);
    g_free(c->key);
    tl_free(c->body);
}

static const ElClass CODE_CLASS = { code_layout, code_draw, code_hit, code_texts, NULL, code_destroy };

El *el_code(const char *lang, const char *code, const char *key) {
    CodeEl *c = g_new0(CodeEl, 1);
    c->base.k = &CODE_CLASS;
    c->lang = g_strdup(lang ? lang : "");
    c->code = g_strdup(code ? code : "");
    c->key = g_strdup(key);
    c->body = tl_new(syntax_highlight(c->code, c->lang, F_MONO), 5);
    c->body->char_wrap = TRUE;
    return &c->base;
}

/* ======================= table ======================= */

typedef struct {
    El base;
    int cols, nrows; /* nrows includes header */
    TextLayout **cells; /* nrows * cols */
    double *colx, *colw, *rowy, *rowh;
} TableEl;

#define TPX 12
#define TPY 8

static void table_layout(El *e, double w) {
    TableEl *t = (TableEl *)e;
    int C = t->cols;
    if (C == 0) { e->w = 0; e->h = 0; return; }
    double *natural = g_new(double, C), *minw = g_new(double, C);
    for (int c = 0; c < C; c++) { natural[c] = 30; minw[c] = 40; }
    for (int r = 0; r < t->nrows; r++)
        for (int c = 0; c < C; c++) {
            TextLayout *tl = t->cells[r * C + c];
            tl_layout(tl, -1);
            natural[c] = MAX(natural[c], tl->w + TPX * 2);
            minw[c] = MAX(minw[c], MIN(180, tl_longest_word(tl) + TPX * 2));
        }
    double total = 0, mintotal = 0;
    for (int c = 0; c < C; c++) { total += natural[c]; mintotal += minw[c]; }
    for (int c = 0; c < C; c++) {
        if (total <= w) t->colw[c] = natural[c];
        else if (mintotal >= w) t->colw[c] = minw[c] * w / mintotal;
        else {
            double flex_total = 0;
            for (int k = 0; k < C; k++) flex_total += MAX(0, natural[k] - minw[k]);
            t->colw[c] = minw[c] + (w - mintotal) * MAX(0, natural[c] - minw[c]) / MAX(1, flex_total);
        }
    }
    double x = 0;
    for (int c = 0; c < C; c++) { t->colx[c] = x; x += t->colw[c]; }
    double y = 0;
    for (int r = 0; r < t->nrows; r++) {
        double h = 0;
        for (int c = 0; c < C; c++) {
            TextLayout *tl = t->cells[r * C + c];
            tl_layout(tl, t->colw[c] - TPX * 2);
            h = MAX(h, tl->h);
        }
        t->rowy[r] = y;
        t->rowh[r] = h + TPY * 2;
        y += h + TPY * 2;
    }
    g_free(natural);
    g_free(minw);
    e->w = MIN(w, x);
    e->h = y;
}

static void table_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    TableEl *t = (TableEl *)e;
    if (!t->cols) return;
    cairo_save(cr);
    rounded_rect(cr, x, y, e->w, e->h, 10);
    cairo_clip(cr);
    set_color(cr, C_CODE_HEADER);
    cairo_rectangle(cr, x, y, e->w, t->rowh[0]);
    cairo_fill(cr);
    set_color(cr, C_SEPARATOR);
    for (int r = 1; r < t->nrows; r++) cairo_rectangle(cr, x, y + t->rowy[r], e->w, 1);
    for (int c = 1; c < t->cols; c++) cairo_rectangle(cr, x + t->colx[c], y, 1, e->h);
    cairo_fill(cr);
    cairo_restore(cr);
    stroke_rounded(cr, x, y, e->w, e->h, 10, th(C_CARD_BORDER), 1);
    for (int r = 0; r < t->nrows; r++) {
        double ry = y + t->rowy[r];
        if (ry > env->clip_bottom) break;
        if (ry + t->rowh[r] < env->clip_top) continue;
        for (int c = 0; c < t->cols; c++) draw_tl(t->cells[r * t->cols + c], cr, x + t->colx[c] + TPX, ry + TPY, env);
    }
}

static gboolean table_hit(El *e, double x, double y, HitResult *r) {
    TableEl *t = (TableEl *)e;
    for (int row = 0; row < t->nrows; row++) {
        if (y < t->rowy[row] || y >= t->rowy[row] + t->rowh[row]) continue;
        for (int c = 0; c < t->cols; c++) {
            if (x < t->colx[c] || x >= t->colx[c] + t->colw[c]) continue;
            const char *link = tl_link_at(t->cells[row * t->cols + c], x - t->colx[c] - TPX, y - t->rowy[row] - TPY);
            if (!link) return FALSE;
            Hit h = hit_make(HIT_LINK, link, NULL, NULL, 0);
            char *key = g_strconcat("link:", link, NULL);
            set_hit(r, &h, 0, 0, 0, 0, key);
            g_free(key);
            hit_free_contents(&h);
            return TRUE;
        }
    }
    return FALSE;
}

static void table_texts(El *e, double x, double y, GArray *refs) {
    TableEl *t = (TableEl *)e;
    for (int r = 0; r < t->nrows; r++)
        for (int c = 0; c < t->cols; c++) add_ref(refs, t->cells[r * t->cols + c], x + t->colx[c] + TPX, y + t->rowy[r] + TPY);
}

static void table_destroy(El *e) {
    TableEl *t = (TableEl *)e;
    for (int i = 0; i < t->nrows * t->cols; i++) tl_free(t->cells[i]);
    g_free(t->cells);
    g_free(t->colx);
    g_free(t->colw);
    g_free(t->rowy);
    g_free(t->rowh);
}

static const ElClass TABLE_CLASS = { table_layout, table_draw, table_hit, table_texts, NULL, table_destroy };

El *el_table(GPtrArray *header, GPtrArray *rows, const int *aligns, int ncols) {
    TableEl *t = g_new0(TableEl, 1);
    t->base.k = &TABLE_CLASS;
    int cols = MAX((int)header->len, ncols);
    for (guint r = 0; r < rows->len; r++) cols = MAX(cols, (int)((GPtrArray *)g_ptr_array_index(rows, r))->len);
    t->cols = cols;
    t->nrows = rows->len + 1;
    t->cells = g_new0(TextLayout *, t->nrows * cols);
    for (int r = 0; r < t->nrows; r++) {
        GPtrArray *src = r == 0 ? header : g_ptr_array_index(rows, r - 1);
        for (int c = 0; c < cols; c++) {
            RichText *rt = c < (int)src->len ? src->pdata[c] : NULL;
            if (c < (int)src->len) src->pdata[c] = NULL;
            if (!rt) rt = rt_plain("", F_BODY, 0, C_TEXT);
            TextLayout *tl = tl_new(rt, 4);
            if (aligns && c < ncols) tl->align = aligns[c];
            t->cells[r * cols + c] = tl;
        }
    }
    g_ptr_array_unref(header);
    g_ptr_array_unref(rows);
    t->colx = g_new0(double, MAX(1, cols));
    t->colw = g_new0(double, MAX(1, cols));
    t->rowy = g_new0(double, t->nrows);
    t->rowh = g_new0(double, t->nrows);
    return &t->base;
}

/* ======================= list ======================= */

typedef struct {
    El base;
    GPtrArray *markers;
    GArray *tasks;
    GPtrArray *contents;
    FontId font;
    ColorId color;
    double marker_w;
    double *offsets;
} ListEl;

static void list_layout(El *e, double w) {
    ListEl *l = (ListEl *)e;
    double longest = 10;
    for (guint i = 0; i < l->markers->len; i++) {
        int task = g_array_index(l->tasks, int, i);
        const char *m = g_ptr_array_index(l->markers, i);
        longest = MAX(longest, task >= 0 ? 22 : (strcmp(m, "•") && strcmp(m, "◦") ? text_width(m, l->font, 0) : 10));
    }
    l->marker_w = MAX(20, longest + 8);
    g_free(l->offsets);
    l->offsets = g_new(double, MAX(1, l->contents->len));
    double y = 0;
    for (guint i = 0; i < l->contents->len; i++) {
        if (i) y += 5;
        l->offsets[i] = y;
        El *c = g_ptr_array_index(l->contents, i);
        el_layout(c, w - l->marker_w);
        y += c->h;
    }
    e->w = w;
    e->h = y;
}

static void list_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    ListEl *l = (ListEl *)e;
    double line_h = theme_font_px(l->font) * 1.25;
    for (guint i = 0; i < l->contents->len; i++) {
        El *c = g_ptr_array_index(l->contents, i);
        double iy = y + l->offsets[i];
        if (iy > env->clip_bottom) break;
        if (iy + c->h < env->clip_top) continue;
        int task = g_array_index(l->tasks, int, i);
        const char *m = g_ptr_array_index(l->markers, i);
        if (task >= 0) {
            double by = iy + (line_h - 14) / 2 + 1;
            if (task) {
                fill_rounded(cr, x + 3, by, 14, 14, 4, th(C_ACCENT));
                draw_check(cr, x + 3, by, 14, 14, th(C_ON_ACCENT), 1.6);
            } else stroke_rounded(cr, x + 3, by, 14, 14, 4, th(C_TEXT3), 1.2);
        } else if (!strcmp(m, "•")) {
            set_rgba(cr, rgba_alpha(th(l->color), 0.7));
            cairo_new_path(cr);
            cairo_arc(cr, x + 8.5, iy + line_h / 2 + 0.5, 2.5, 0, 2 * G_PI);
            cairo_fill(cr);
        } else if (!strcmp(m, "◦")) {
            set_rgba(cr, rgba_alpha(th(l->color), 0.7));
            cairo_set_line_width(cr, 1);
            cairo_new_path(cr);
            cairo_arc(cr, x + 8.5, iy + line_h / 2 + 0.5, 2.5, 0, 2 * G_PI);
            cairo_stroke(cr);
        } else {
            double mw = text_width(m, l->font, 0);
            draw_label(cr, m, x + l->marker_w - 6 - mw, iy, mw + 2, line_h, l->font, 0, th(C_TEXT2), PANGO_ALIGN_LEFT);
        }
        el_draw(c, cr, x + l->marker_w, iy, env);
    }
}

static gboolean list_hit(El *e, double x, double y, HitResult *r) {
    ListEl *l = (ListEl *)e;
    for (guint i = 0; i < l->contents->len; i++) {
        El *c = g_ptr_array_index(l->contents, i);
        if (y >= l->offsets[i] && y < l->offsets[i] + c->h) {
            if (!el_hit(c, x - l->marker_w, y - l->offsets[i], r)) return FALSE;
            hit_result_offset(r, l->marker_w, l->offsets[i]);
            return TRUE;
        }
    }
    return FALSE;
}

static void list_texts(El *e, double x, double y, GArray *refs) {
    ListEl *l = (ListEl *)e;
    for (guint i = 0; i < l->contents->len; i++) el_texts(g_ptr_array_index(l->contents, i), x + l->marker_w, y + l->offsets[i], refs);
}

static void list_destroy(El *e) {
    ListEl *l = (ListEl *)e;
    g_ptr_array_unref(l->markers);
    g_array_unref(l->tasks);
    g_ptr_array_unref(l->contents);
    g_free(l->offsets);
}

static const ElClass LIST_CLASS = { list_layout, list_draw, list_hit, list_texts, NULL, list_destroy };

El *el_list(GPtrArray *markers, GArray *tasks, GPtrArray *contents, FontId f, ColorId c) {
    ListEl *l = g_new0(ListEl, 1);
    l->base.k = &LIST_CLASS;
    g_ptr_array_set_free_func(markers, g_free);
    g_ptr_array_set_free_func(contents, el_free_cb);
    l->markers = markers;
    l->tasks = tasks;
    l->contents = contents;
    l->font = f;
    l->color = c;
    return &l->base;
}

/* ======================= quote ======================= */

typedef struct { El base; El *child; } QuoteEl;

static void quote_layout(El *e, double w) {
    QuoteEl *q = (QuoteEl *)e;
    el_layout(q->child, w - 16);
    e->w = w;
    e->h = q->child->h;
}
static void quote_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    fill_rounded(cr, x, y, 3, e->h, 1.5, th(C_SEPARATOR));
    el_draw(((QuoteEl *)e)->child, cr, x + 16, y, env);
}
static gboolean quote_hit(El *e, double x, double y, HitResult *r) {
    if (!el_hit(((QuoteEl *)e)->child, x - 16, y, r)) return FALSE;
    hit_result_offset(r, 16, 0);
    return TRUE;
}
static void quote_texts(El *e, double x, double y, GArray *refs) { el_texts(((QuoteEl *)e)->child, x + 16, y, refs); }
static void quote_destroy(El *e) { el_free(((QuoteEl *)e)->child); }
static const ElClass QUOTE_CLASS = { quote_layout, quote_draw, quote_hit, quote_texts, NULL, quote_destroy };

El *el_quote(El *child) {
    QuoteEl *q = g_new0(QuoteEl, 1);
    q->base.k = &QUOTE_CLASS;
    q->child = child;
    return &q->base;
}

/* ======================= image ======================= */

typedef struct {
    El base;
    char *path, *alt;
    cairo_surface_t *surface;
    int pw, ph;
    gboolean loaded;
    double dw;
} ImageEl;

static GHashTable *image_cache; /* path -> cairo_surface_t* */

static void image_load(ImageEl *im) {
    if (im->loaded) return;
    im->loaded = TRUE;
    if (!image_cache) image_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)cairo_surface_destroy);
    cairo_surface_t *s = g_hash_table_lookup(image_cache, im->path);
    if (!s && !strstr(im->path, "://")) {
        int w = 0, h = 0;
        if (gdk_pixbuf_get_file_info(im->path, &w, &h)) {
            GdkPixbuf *pb = (w > 1600 || h > 1600) ? gdk_pixbuf_new_from_file_at_scale(im->path, 1600, 1600, TRUE, NULL) : gdk_pixbuf_new_from_file(im->path, NULL);
            if (pb) {
                GdkPixbuf *o = gdk_pixbuf_apply_embedded_orientation(pb);
                g_object_unref(pb);
                s = gdk_cairo_surface_create_from_pixbuf(o, 1, NULL);
                g_object_unref(o);
                if (g_hash_table_size(image_cache) > 64) g_hash_table_remove_all(image_cache);
                g_hash_table_insert(image_cache, g_strdup(im->path), s);
            }
        }
    }
    if (s) {
        im->surface = cairo_surface_reference(s);
        im->pw = cairo_image_surface_get_width(s);
        im->ph = cairo_image_surface_get_height(s);
    }
}

static void image_layout(El *e, double w) {
    ImageEl *im = (ImageEl *)e;
    image_load(im);
    e->w = w;
    if (im->surface) {
        im->dw = MIN(MIN(w, im->pw / 1.5), 640);
        im->dw = MAX(im->dw, MIN(w, 48));
        e->h = ceil(im->dw * im->ph / MAX(1, im->pw));
    } else e->h = 36;
}

static void image_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    ImageEl *im = (ImageEl *)e;
    if (im->surface) {
        cairo_save(cr);
        rounded_rect(cr, x, y, im->dw, e->h, 10);
        cairo_clip(cr);
        cairo_translate(cr, x, y);
        cairo_scale(cr, im->dw / im->pw, e->h / im->ph);
        cairo_set_source_surface(cr, im->surface, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
        stroke_rounded(cr, x, y, im->dw, e->h, 10, th(C_CODE_BORDER), 1);
    } else {
        double w = MIN(e->w, 360);
        fill_rounded(cr, x, y, w, 36, 8, th(C_CODE_BG));
        draw_icon(cr, "image-x-generic-symbolic", x + 8, y, 20, 36, 14, th(C_TEXT3));
        draw_label(cr, *im->alt ? im->alt : path_base(im->path), x + 32, y, w - 40, 36, F_SMALL, 0, th(C_TEXT2), PANGO_ALIGN_LEFT);
    }
}

static gboolean image_hit(El *e, double x, double y, HitResult *r) {
    ImageEl *im = (ImageEl *)e;
    if (x > (im->surface ? im->dw : MIN(e->w, 360))) return FALSE;
    Hit h = hit_make(HIT_OPEN_FILE, im->path, NULL, NULL, 0);
    char *key = g_strconcat("img:", im->path, NULL);
    set_hit(r, &h, 0, 0, im->surface ? im->dw : 360, e->h, key);
    g_free(key);
    hit_free_contents(&h);
    return TRUE;
}

static void image_destroy(El *e) {
    ImageEl *im = (ImageEl *)e;
    if (im->surface) cairo_surface_destroy(im->surface);
    g_free(im->path);
    g_free(im->alt);
}

static const ElClass IMAGE_CLASS = { image_layout, image_draw, image_hit, NULL, NULL, image_destroy };

El *el_image(const char *path, const char *alt) {
    ImageEl *im = g_new0(ImageEl, 1);
    im->base.k = &IMAGE_CLASS;
    im->path = g_str_has_prefix(path, "file://") ? g_strdup(path + 7) : g_strdup(path);
    im->alt = g_strdup(alt ? alt : "");
    return &im->base;
}

/* ======================= user bubble ======================= */

typedef struct {
    El base;
    TextLayout *tl;
    char *key, *time, *raw;
    gboolean faded;
    double bx, bw, bh;
} BubbleEl;

#define BUB_PX 16
#define BUB_PY 9
#define BUB_META 26

static void bubble_layout(El *e, double w) {
    BubbleEl *b = (BubbleEl *)e;
    double maxw = MIN(w * 0.8, 620);
    tl_layout(b->tl, maxw - BUB_PX * 2);
    b->bw = MAX(b->tl->w, 20) + BUB_PX * 2;
    b->bh = b->tl->h + BUB_PY * 2;
    b->bx = w - b->bw;
    e->w = w;
    e->h = b->bh + BUB_META;
}

static void bubble_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    BubbleEl *b = (BubbleEl *)e;
    RGBA bg = th(C_USER_BUBBLE);
    if (b->faded) bg = rgba_alpha(bg, 0.5);
    fill_rounded(cr, x + b->bx, y, b->bw, b->bh, MIN(20, b->bh / 2), bg);
    draw_tl(b->tl, cr, x + b->bx + BUB_PX, y + BUB_PY, env);
    char *ck = g_strconcat(b->key, ".copy", NULL);
    if (!g_strcmp0(env->hover_item, b->key) || !g_strcmp0(env->hover_key, ck)) {
        double cx = x + b->bx + b->bw - 28, cy = y + b->bh + 3;
        if (!g_strcmp0(env->hover_key, ck)) fill_rounded(cr, cx, cy, 24, 22, 6, th(C_HOVER));
        if (!g_strcmp0(env->copied_key, b->key)) draw_check(cr, cx + 5, cy + 4, 14, 14, th(C_TEXT3), 1.5);
        else draw_icon(cr, "edit-copy-symbolic", cx, cy, 24, 22, 12, th(C_TEXT3));
        draw_label(cr, b->time, cx - 70, cy, 64, 22, F_CAPTION, 0, th(C_TEXT3), PANGO_ALIGN_RIGHT);
    }
    g_free(ck);
}

static gboolean bubble_hit(El *e, double x, double y, HitResult *r) {
    BubbleEl *b = (BubbleEl *)e;
    double cx = b->bx + b->bw - 28, cy = b->bh + 3;
    if (x >= cx && x <= cx + 24 && y >= cy && y <= cy + 22) {
        Hit h = hit_make(HIT_COPY, b->raw, NULL, NULL, 0);
        char *k = g_strconcat(b->key, ".copy", NULL);
        set_hit(r, &h, cx, cy, 24, 22, k);
        g_free(k);
        hit_free_contents(&h);
        /* the copied flash is keyed by the bubble */
        g_free(r->hit.b);
        r->hit.b = g_strdup(b->key);
        return TRUE;
    }
    const char *link = tl_link_at(b->tl, x - b->bx - BUB_PX, y - BUB_PY);
    if (link) {
        Hit h = hit_make(HIT_LINK, link, NULL, NULL, 0);
        char *k = g_strconcat("link:", link, NULL);
        set_hit(r, &h, 0, 0, 0, 0, k);
        g_free(k);
        hit_free_contents(&h);
        return TRUE;
    }
    return FALSE;
}

static void bubble_texts(El *e, double x, double y, GArray *refs) {
    BubbleEl *b = (BubbleEl *)e;
    add_ref(refs, b->tl, x + b->bx + BUB_PX, y + BUB_PY);
}

static void bubble_destroy(El *e) {
    BubbleEl *b = (BubbleEl *)e;
    tl_free(b->tl);
    g_free(b->key);
    g_free(b->time);
    g_free(b->raw);
}

static const ElClass BUBBLE_CLASS = { bubble_layout, bubble_draw, bubble_hit, bubble_texts, NULL, bubble_destroy };

El *el_user_bubble(const char *text, const char *time, const char *key, gboolean faded, gboolean notice) {
    BubbleEl *b = g_new0(BubbleEl, 1);
    b->base.k = &BUBBLE_CLASS;
    b->tl = tl_new(rt_plain(text, F_BODY, 0, faded ? C_TEXT3 : C_TEXT), 5);
    b->key = g_strdup(key);
    b->time = g_strdup(time);
    b->raw = g_strdup(text);
    b->faded = faded;
    return &b->base;
}

/* ======================= turn header ======================= */

typedef struct {
    El base;
    char *label;
    ColorId color;
    gboolean spinning;
    double live_since;
} HeaderEl;

static void header_layout(El *e, double w) { e->w = w; e->h = 40; }

static void header_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    HeaderEl *h = (HeaderEl *)e;
    double tx = x;
    if (h->spinning) {
        draw_spinner(cr, tx + 6, y + 15, 5, env->phase, th(C_TEXT3));
        tx += 18;
    }
    char *text;
    if (h->live_since > 0) {
        char *d = fmt_duration(now_ts() - h->live_since);
        text = g_strdup_printf("%s · %s", h->label, d);
        g_free(d);
    } else text = g_strdup(h->label);
    draw_label(cr, text, tx, y + 3, e->w - (tx - x), 24, F_SMALL, 0, th(h->color), PANGO_ALIGN_LEFT);
    g_free(text);
    set_color(cr, C_SEPARATOR);
    cairo_rectangle(cr, x, y + 39, e->w, 1);
    cairo_fill(cr);
}

static gboolean header_anim(El *e) { return ((HeaderEl *)e)->spinning; }
static void header_destroy(El *e) { g_free(((HeaderEl *)e)->label); }
static const ElClass HEADER_CLASS = { header_layout, header_draw, NULL, NULL, header_anim, header_destroy };

El *el_turn_header(const char *label, ColorId color, gboolean spinning, double live_since) {
    HeaderEl *h = g_new0(HeaderEl, 1);
    h->base.k = &HEADER_CLASS;
    h->label = g_strdup(label);
    h->color = color;
    h->spinning = spinning;
    h->live_since = live_since;
    return &h->base;
}

/* ======================= row ======================= */

typedef struct {
    El base;
    char *icon, *title, *subtitle, *key;
    Trailing tr;
    Hit hit;
    int expanded;
    ColorId title_color;
    FontId title_font;
    double height;
} RowEl;

static void row_layout(El *e, double w) { e->w = w; e->h = ((RowEl *)e)->height; }

static double trailing_width(Trailing *t) {
    switch (t->kind) {
    case TR_SPINNER: return 20;
    case TR_TEXT: return text_width(t->text, F_CAPTION, 0) + 8;
    case TR_DIFF: {
        char *s = g_strdup_printf("+%d −%d", t->added, t->removed);
        double w = text_width(s, F_CAPTION, 0) + 8;
        g_free(s);
        return w;
    }
    case TR_ICON: return 20;
    default: return 0;
    }
}

static void row_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    RowEl *r = (RowEl *)e;
    gboolean hovered = !g_strcmp0(env->hover_key, r->key);
    if (hovered && r->hit.kind != HIT_NONE) fill_rounded(cr, x - 6, y + 1, e->w + 12, e->h - 2, 8, th(C_HOVER));
    gboolean chevron = r->expanded >= 0 && (hovered || r->expanded == 1);
    if (chevron) draw_chevron(cr, x + 8, y + e->h / 2, 8, r->expanded == 1 ? 1 : 0, th(C_TEXT3));
    else draw_icon(cr, r->icon, x, y, 16, e->h, 14, th(C_TEXT3));
    double tx = x + 24;
    double tw_tr = trailing_width(&r->tr);
    double avail = e->w - (tx - x) - tw_tr;
    double tw = MIN(text_width(r->title, r->title_font, 0), avail);
    draw_label(cr, r->title, tx, y, tw + 1, e->h, r->title_font, 0, th(r->title_color), PANGO_ALIGN_LEFT);
    tx += tw;
    if (r->subtitle && *r->subtitle && avail - tw > 30) {
        char *sub = g_strconcat("  ·  ", r->subtitle, NULL);
        for (char *p = sub; *p; p++)
            if (*p == '\n') *p = ' ';
        draw_label(cr, sub, tx, y, avail - tw, e->h, F_SMALL, 0, th(C_TEXT3), PANGO_ALIGN_LEFT);
        g_free(sub);
    }
    double trx = x + e->w - tw_tr;
    switch (r->tr.kind) {
    case TR_SPINNER: draw_spinner(cr, x + e->w - 8, y + e->h / 2, 5, env->phase, th(C_TEXT3)); break;
    case TR_TEXT: draw_label(cr, r->tr.text, trx, y, tw_tr, e->h, F_CAPTION, 0, th(r->tr.color), PANGO_ALIGN_RIGHT); break;
    case TR_DIFF: {
        char *a = g_strdup_printf("+%d", r->tr.added), *d = g_strdup_printf(" −%d", r->tr.removed);
        double wa = text_width(a, F_CAPTION, 0);
        draw_label(cr, a, trx + 8, y, wa + 2, e->h, F_CAPTION, 0, th(C_DIFF_ADD_TEXT), PANGO_ALIGN_LEFT);
        draw_label(cr, d, trx + 8 + wa, y, tw_tr, e->h, F_CAPTION, 0, th(C_DIFF_REM_TEXT), PANGO_ALIGN_LEFT);
        g_free(a);
        g_free(d);
        break;
    }
    case TR_ICON: draw_icon(cr, r->tr.text, x + e->w - 16, y, 16, e->h, 12, th(r->tr.color)); break;
    default: break;
    }
}

static gboolean row_hit(El *e, double x, double y, HitResult *res) {
    RowEl *r = (RowEl *)e;
    if (r->hit.kind == HIT_NONE) return FALSE;
    return set_hit(res, &r->hit, -6, 1, e->w + 12, e->h - 2, r->key);
}

static gboolean row_anim(El *e) { return ((RowEl *)e)->tr.kind == TR_SPINNER; }

static void row_destroy(El *e) {
    RowEl *r = (RowEl *)e;
    g_free(r->icon);
    g_free(r->title);
    g_free(r->subtitle);
    g_free(r->key);
    g_free(r->tr.text);
    hit_free_contents(&r->hit);
}

static const ElClass ROW_CLASS = { row_layout, row_draw, row_hit, NULL, row_anim, row_destroy };

El *el_row(const char *icon, const char *title, const char *subtitle, Trailing tr, const char *key, Hit hit, int expanded, ColorId title_color,
           FontId title_font, double height) {
    RowEl *r = g_new0(RowEl, 1);
    r->base.k = &ROW_CLASS;
    r->icon = g_strdup(icon);
    r->title = g_strdup(title);
    r->subtitle = g_strdup(subtitle);
    r->key = g_strdup(key);
    r->tr = tr; /* takes tr.text */
    r->hit = hit; /* takes ownership */
    r->expanded = expanded;
    r->title_color = title_color;
    r->title_font = title_font;
    r->height = height;
    return &r->base;
}

/* ======================= buttons ======================= */

typedef struct {
    El base;
    char *title, *icon, *key;
    ButtonStyle style;
    Hit hit;
} ButtonEl;

static double button_intrinsic(ButtonEl *b) { return text_width(b->title, F_SMALL, 0) + (b->icon ? 44 : 26); }

static void button_layout(El *e, double w) {
    e->w = MIN(w, button_intrinsic((ButtonEl *)e));
    e->h = 30;
}

static void button_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    ButtonEl *b = (ButtonEl *)e;
    gboolean hovered = !g_strcmp0(env->hover_key, b->key);
    RGBA fg = th(C_TEXT);
    switch (b->style) {
    case BTN_PRIMARY:
        fill_rounded(cr, x, y, e->w, e->h, 9, hovered ? rgba_mix(th(C_ACCENT), (RGBA){ 0, 0, 0, 1 }, 0.12) : th(C_ACCENT));
        fg = th(C_ON_ACCENT);
        break;
    case BTN_SECONDARY: fill_rounded(cr, x, y, e->w, e->h, 9, th(hovered ? C_PRESSED : C_HOVER)); break;
    case BTN_PLAIN:
        if (hovered) fill_rounded(cr, x, y, e->w, e->h, 9, th(C_HOVER));
        fg = th(C_TEXT2);
        break;
    case BTN_DANGER:
        fill_rounded(cr, x, y, e->w, e->h, 9, hovered ? rgba_mix(th(C_ERROR_SOFT), th(C_ERROR), 0.2) : th(C_ERROR_SOFT));
        fg = th(C_ERROR);
        break;
    }
    double tx = x + 13;
    if (b->icon) {
        if (!strcmp(b->icon, "check")) draw_check(cr, tx, y + 8, 14, 14, fg, 1.6);
        else draw_icon(cr, b->icon, tx, y, 14, e->h, 12, fg);
        tx += 18;
    }
    draw_label(cr, b->title, tx, y, x + e->w - tx - 8, e->h, F_SMALL, FS_MEDIUM, fg, PANGO_ALIGN_LEFT);
}

static gboolean button_hit(El *e, double x, double y, HitResult *r) {
    ButtonEl *b = (ButtonEl *)e;
    return set_hit(r, &b->hit, 0, 0, e->w, e->h, b->key);
}

static void button_destroy(El *e) {
    ButtonEl *b = (ButtonEl *)e;
    g_free(b->title);
    g_free(b->icon);
    g_free(b->key);
    hit_free_contents(&b->hit);
}

static const ElClass BUTTON_CLASS = { button_layout, button_draw, button_hit, NULL, NULL, button_destroy };

El *el_button(const char *title, const char *icon, ButtonStyle style, Hit hit, const char *key) {
    ButtonEl *b = g_new0(ButtonEl, 1);
    b->base.k = &BUTTON_CLASS;
    b->title = g_strdup(title);
    b->icon = g_strdup(icon);
    b->style = style;
    b->hit = hit;
    b->key = g_strdup(key);
    return &b->base;
}

typedef struct { El base; GPtrArray *buttons; double *xs; } ButtonRowEl;

static void brow_layout(El *e, double w) {
    ButtonRowEl *r = (ButtonRowEl *)e;
    g_free(r->xs);
    r->xs = g_new(double, MAX(1, r->buttons->len));
    double x = 0;
    for (guint i = 0; i < r->buttons->len; i++) {
        El *b = g_ptr_array_index(r->buttons, i);
        r->xs[i] = x;
        el_layout(b, w);
        x += b->w + 8;
    }
    e->w = w;
    e->h = 30;
}

static void brow_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    ButtonRowEl *r = (ButtonRowEl *)e;
    for (guint i = 0; i < r->buttons->len; i++) el_draw(g_ptr_array_index(r->buttons, i), cr, x + r->xs[i], y, env);
}

static gboolean brow_hit(El *e, double x, double y, HitResult *res) {
    ButtonRowEl *r = (ButtonRowEl *)e;
    for (guint i = 0; i < r->buttons->len; i++) {
        El *b = g_ptr_array_index(r->buttons, i);
        if (x >= r->xs[i] && x < r->xs[i] + b->w) {
            if (!el_hit(b, x - r->xs[i], y, res)) return FALSE;
            hit_result_offset(res, r->xs[i], 0);
            return TRUE;
        }
    }
    return FALSE;
}

static void brow_destroy(El *e) {
    ButtonRowEl *r = (ButtonRowEl *)e;
    g_ptr_array_unref(r->buttons);
    g_free(r->xs);
}

static const ElClass BROW_CLASS = { brow_layout, brow_draw, brow_hit, NULL, NULL, brow_destroy };

El *el_button_row(GPtrArray *buttons) {
    ButtonRowEl *r = g_new0(ButtonRowEl, 1);
    r->base.k = &BROW_CLASS;
    g_ptr_array_set_free_func(buttons, el_free_cb);
    r->buttons = buttons;
    return &r->base;
}

/* ======================= footer ======================= */

typedef struct { El base; char *copy, *usage, *time, *key; } FooterEl;

static void footer_layout(El *e, double w) { e->w = w; e->h = 32; }

static void footer_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    FooterEl *f = (FooterEl *)e;
    double cx = x - 4, cy = y + 4;
    if (!g_strcmp0(env->hover_key, f->key)) fill_rounded(cr, cx, cy, 26, 24, 6, th(C_HOVER));
    if (!g_strcmp0(env->copied_key, f->key)) draw_check(cr, cx + 6, cy + 5, 14, 14, th(C_TEXT3), 1.5);
    else draw_icon(cr, "edit-copy-symbolic", cx, cy, 26, 24, 12, th(C_TEXT3));
    double tx = x + 34;
    if (f->usage && *f->usage) {
        draw_icon(cr, "drive-harddisk-system-symbolic", tx, y + 4, 14, 24, 11, th(C_TEXT3));
        tx += 20;
        char *s = g_strconcat("Usage ", f->usage, NULL);
        draw_label(cr, s, tx, y + 4, 200, 24, F_CAPTION, 0, th(C_TEXT3), PANGO_ALIGN_LEFT);
        tx += text_width(s, F_CAPTION, 0) + 16;
        g_free(s);
    }
    draw_label(cr, f->time, tx, y + 4, 100, 24, F_CAPTION, 0, th(C_TEXT3), PANGO_ALIGN_LEFT);
}

static gboolean footer_hit(El *e, double x, double y, HitResult *r) {
    FooterEl *f = (FooterEl *)e;
    if (x >= -4 && x <= 22 && y >= 4 && y <= 28) {
        Hit h = hit_make(HIT_COPY, f->copy, f->key, NULL, 0);
        set_hit(r, &h, -4, 4, 26, 24, f->key);
        hit_free_contents(&h);
        return TRUE;
    }
    return FALSE;
}

static void footer_destroy(El *e) {
    FooterEl *f = (FooterEl *)e;
    g_free(f->copy);
    g_free(f->usage);
    g_free(f->time);
    g_free(f->key);
}

static const ElClass FOOTER_CLASS = { footer_layout, footer_draw, footer_hit, NULL, NULL, footer_destroy };

El *el_footer(const char *copy_text, const char *usage, const char *time, const char *key) {
    FooterEl *f = g_new0(FooterEl, 1);
    f->base.k = &FOOTER_CLASS;
    f->copy = g_strdup(copy_text);
    f->usage = g_strdup(usage);
    f->time = g_strdup(time);
    f->key = g_strdup(key);
    return &f->base;
}

/* ======================= diff ======================= */

typedef struct {
    El base;
    TextLayout *tl;
    GArray *kinds;
    GArray *starts; /* byte offset where each source line starts */
} DiffEl;

static void diff_layout(El *e, double w) {
    DiffEl *d = (DiffEl *)e;
    tl_layout(d->tl, w - 16);
    e->w = w;
    e->h = d->tl->h + 12;
}

static void diff_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    DiffEl *d = (DiffEl *)e;
    fill_rounded(cr, x, y, e->w, e->h, 10, th(C_CODE_BG));
    if (d->tl->layout) {
        cairo_save(cr);
        rounded_rect(cr, x, y, e->w, e->h, 10);
        cairo_clip(cr);
        PangoLayoutIter *it = pango_layout_get_iter(d->tl->layout);
        do {
            PangoLayoutLine *line = pango_layout_iter_get_line_readonly(it);
            PangoRectangle lr;
            pango_layout_iter_get_line_extents(it, NULL, &lr);
            double top = y + 6 + lr.y / (double)PANGO_SCALE, h = lr.height / (double)PANGO_SCALE;
            if (top > env->clip_bottom) break;
            if (top + h < env->clip_top) continue;
            int lo = 0, hi = d->starts->len - 1;
            while (lo < hi) {
                int mid = (lo + hi + 1) / 2;
                if (g_array_index(d->starts, int, mid) <= line->start_index) lo = mid; else hi = mid - 1;
            }
            int kind = lo < (int)d->kinds->len ? g_array_index(d->kinds, int, lo) : 0;
            if (kind) {
                set_color(cr, kind > 0 ? C_DIFF_ADD : C_DIFF_REM);
                cairo_rectangle(cr, x + 1, top - 1.5, e->w - 2, h + 3);
                cairo_fill(cr);
            }
        } while (pango_layout_iter_next_line(it));
        pango_layout_iter_free(it);
        cairo_restore(cr);
    }
    stroke_rounded(cr, x, y, e->w, e->h, 10, th(C_CODE_BORDER), 1);
    draw_tl(d->tl, cr, x + 8, y + 6, env);
}

static void diff_texts(El *e, double x, double y, GArray *refs) { add_ref(refs, ((DiffEl *)e)->tl, x + 8, y + 6); }

static void diff_destroy(El *e) {
    DiffEl *d = (DiffEl *)e;
    tl_free(d->tl);
    g_array_unref(d->kinds);
    g_array_unref(d->starts);
}

static const ElClass DIFF_CLASS = { diff_layout, diff_draw, NULL, diff_texts, NULL, diff_destroy };

El *el_diff(GPtrArray *lines, GArray *kinds) {
    DiffEl *d = g_new0(DiffEl, 1);
    d->base.k = &DIFF_CLASS;
    d->kinds = kinds;
    d->starts = g_array_new(FALSE, FALSE, sizeof(int));
    RichText *rt = rt_new();
    for (guint i = 0; i < lines->len; i++) {
        int k = g_array_index(kinds, int, i);
        int start = rt->text->len;
        g_array_append_val(d->starts, start);
        char *s = g_strconcat(k < 0 ? "- " : k > 0 ? "+ " : "  ", (char *)g_ptr_array_index(lines, i), i + 1 < lines->len ? "\n" : "", NULL);
        rt_append(rt, s, -1, F_MONO_SMALL, 0, k < 0 ? C_DIFF_REM_TEXT : k > 0 ? C_DIFF_ADD_TEXT : C_TEXT2);
        g_free(s);
    }
    if (!lines->len) rt_append(rt, " ", 1, F_MONO_SMALL, 0, C_TEXT2);
    g_ptr_array_unref(lines);
    d->tl = tl_new(rt, 3);
    d->tl->char_wrap = TRUE;
    return &d->base;
}

/* ======================= option ======================= */

typedef struct {
    El base;
    char *label, *key;
    TextLayout *detail;
    gboolean selected, multi;
    Hit hit;
} OptionEl;

static void option_layout(El *e, double w) {
    OptionEl *o = (OptionEl *)e;
    double dh = 0;
    if (o->detail) { tl_layout(o->detail, w - 44); dh = o->detail->h; }
    e->w = w;
    e->h = 34 + (dh > 0 ? dh + 4 : 0);
}

static void option_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    OptionEl *o = (OptionEl *)e;
    RGBA bg = o->selected ? th(C_ACCENT_SOFT) : !g_strcmp0(env->hover_key, o->key) ? th(C_HOVER) : th(C_CLEAR);
    fill_rounded(cr, x, y, e->w, e->h, 10, bg);
    stroke_rounded(cr, x, y, e->w, e->h, 10, th(o->selected ? C_ACCENT : C_CARD_BORDER), 1);
    double ix = x + 12, iy = y + 10;
    if (o->multi) {
        if (o->selected) {
            fill_rounded(cr, ix, iy, 14, 14, 4, th(C_ACCENT));
            draw_check(cr, ix, iy, 14, 14, th(C_ON_ACCENT), 1.6);
        } else stroke_rounded(cr, ix, iy, 14, 14, 4, th(C_TEXT3), 1.2);
    } else {
        set_color(cr, o->selected ? C_ACCENT : C_TEXT3);
        cairo_set_line_width(cr, 1.2);
        cairo_new_path(cr);
        cairo_arc(cr, ix + 7, iy + 7, 6.4, 0, 2 * G_PI);
        cairo_stroke(cr);
        if (o->selected) {
            set_color(cr, C_ACCENT);
            cairo_new_path(cr);
            cairo_arc(cr, ix + 7, iy + 7, 3.5, 0, 2 * G_PI);
            cairo_fill(cr);
        }
    }
    draw_label(cr, o->label, x + 34, y + 5, e->w - 44, 24, F_BODY, 0, th(C_TEXT), PANGO_ALIGN_LEFT);
    if (o->detail) draw_tl(o->detail, cr, x + 34, y + 30, env);
}

static gboolean option_hit(El *e, double x, double y, HitResult *r) {
    OptionEl *o = (OptionEl *)e;
    return set_hit(r, &o->hit, 0, 0, e->w, e->h, o->key);
}

static void option_destroy(El *e) {
    OptionEl *o = (OptionEl *)e;
    g_free(o->label);
    g_free(o->key);
    tl_free(o->detail);
    hit_free_contents(&o->hit);
}

static const ElClass OPTION_CLASS = { option_layout, option_draw, option_hit, NULL, NULL, option_destroy };

El *el_option(const char *label, const char *detail, gboolean selected, gboolean multi, Hit hit, const char *key) {
    OptionEl *o = g_new0(OptionEl, 1);
    o->base.k = &OPTION_CLASS;
    o->label = g_strdup(label);
    o->key = g_strdup(key);
    o->detail = detail && *detail ? tl_new(rt_plain(detail, F_CAPTION, 0, C_TEXT3), 2) : NULL;
    o->selected = selected;
    o->multi = multi;
    o->hit = hit;
    return &o->base;
}

/* ======================= file card ======================= */

typedef struct { El base; char *path, *detail, *key; GIcon *gicon; } CardEl;

static void card_layout(El *e, double w) { e->w = MIN(w, 520); e->h = 58; }

static void card_draw(El *e, cairo_t *cr, double x, double y, DrawEnv *env) {
    CardEl *c = (CardEl *)e;
    fill_rounded(cr, x, y, e->w, e->h, 12, th(!g_strcmp0(env->hover_key, c->key) ? C_HOVER : C_CARD_BG));
    stroke_rounded(cr, x, y, e->w, e->h, 12, th(C_CARD_BORDER), 1);
    gboolean drawn = FALSE;
    if (c->gicon) {
        GtkIconInfo *info = gtk_icon_theme_lookup_by_gicon_for_scale(gtk_icon_theme_get_default(), c->gicon, 32, env->scale, GTK_ICON_LOOKUP_FORCE_SIZE);
        if (info) {
            GdkPixbuf *pb = gtk_icon_info_load_icon(info, NULL);
            if (pb) {
                cairo_surface_t *s = gdk_cairo_surface_create_from_pixbuf(pb, env->scale, NULL);
                cairo_set_source_surface(cr, s, x + 12, y + 13);
                cairo_paint(cr);
                cairo_surface_destroy(s);
                g_object_unref(pb);
                drawn = TRUE;
            }
            g_object_unref(info);
        }
    }
    if (!drawn) draw_icon(cr, "text-x-generic-symbolic", x + 12, y + 13, 32, 32, 24, th(C_TEXT2));
    draw_label(cr, path_base(c->path), x + 54, y + 9, e->w - 110, 20, F_BODY, FS_SEMIBOLD, th(C_TEXT), PANGO_ALIGN_LEFT);
    char *sub = *c->detail ? g_strdup(c->detail) : tilde_path(c->path);
    draw_label(cr, sub, x + 54, y + 29, e->w - 110, 18, F_CAPTION, 0, th(C_TEXT3), PANGO_ALIGN_LEFT);
    g_free(sub);
    draw_label(cr, "Open", x + e->w - 56, y, 44, e->h, F_SMALL, 0, th(C_ACCENT), PANGO_ALIGN_RIGHT);
}

static gboolean card_hit(El *e, double x, double y, HitResult *r) {
    CardEl *c = (CardEl *)e;
    Hit h = hit_make(HIT_OPEN_FILE, c->path, NULL, NULL, 0);
    set_hit(r, &h, 0, 0, e->w, e->h, c->key);
    hit_free_contents(&h);
    return TRUE;
}

static void card_destroy(El *e) {
    CardEl *c = (CardEl *)e;
    g_free(c->path);
    g_free(c->detail);
    g_free(c->key);
    if (c->gicon) g_object_unref(c->gicon);
}

static const ElClass CARD_CLASS = { card_layout, card_draw, card_hit, NULL, NULL, card_destroy };

El *el_file_card(const char *path, const char *detail, const char *key) {
    CardEl *c = g_new0(CardEl, 1);
    c->base.k = &CARD_CLASS;
    c->path = g_strdup(path);
    c->detail = g_strdup(detail ? detail : "");
    c->key = g_strdup(key);
    char *ct = g_content_type_guess(path, NULL, 0, NULL);
    if (ct) {
        c->gicon = g_content_type_get_icon(ct);
        g_free(ct);
    }
    return &c->base;
}

/* ======================= markdown render ======================= */

static int key_depth(const char *prefix) {
    int n = 0;
    for (const char *p = prefix; *p; p++)
        if (*p == 'L') n++;
    return n;
}

El *md_render(GPtrArray *blocks, const MDStyle *st, const char *prefix) {
    GPtrArray *out = g_ptr_array_new();
    gboolean prev_heading = FALSE;
    for (guint i = 0; i < blocks->len; i++) {
        MDBlock *b = g_ptr_array_index(blocks, i);
        char *key = g_strdup_printf("%s.%u", prefix, i);
        El *el = NULL;
        double top_extra = 0;
        switch (b->kind) {
        case MD_PARA:
            el = el_text(b->rt, st->spacing);
            b->rt = NULL;
            break;
        case MD_HEADING:
            el = el_text(b->rt, 4);
            b->rt = NULL;
            if (i > 0) top_extra = b->level <= 2 ? 10 : 6;
            break;
        case MD_CODE: el = el_code(b->lang, b->code, key); break;
        case MD_LIST: {
            GPtrArray *markers = g_ptr_array_new();
            GArray *tasks = g_array_new(FALSE, FALSE, sizeof(int));
            GPtrArray *contents = g_ptr_array_new();
            for (guint j = 0; j < b->items->len; j++) {
                MDItem *it = g_ptr_array_index(b->items, j);
                char *m = b->ordered ? g_strdup_printf("%d.", b->start + (int)j) : g_strdup(key_depth(prefix) % 2 == 0 ? "•" : "◦");
                g_ptr_array_add(markers, m);
                g_array_append_val(tasks, it->task);
                char *ck = g_strdup_printf("%sL%u", key, j);
                GPtrArray *blks = it->blocks;
                it->blocks = g_ptr_array_new();
                g_ptr_array_add(contents, md_render(blks, st, ck));
                g_free(ck);
            }
            el = el_list(markers, tasks, contents, st->font, st->color);
            break;
        }
        case MD_QUOTE: {
            char *qk = g_strconcat(key, "q", NULL);
            GPtrArray *ch = b->children;
            b->children = NULL;
            MDStyle qs = *st;
            qs.color = C_TEXT2;
            el = el_quote(md_render(ch, &qs, qk));
            g_free(qk);
            break;
        }
        case MD_TABLE:
            el = el_table(b->header, b->rows, b->aligns, b->ncols);
            b->header = NULL;
            b->rows = NULL;
            break;
        case MD_HR: el = el_rule(6); break;
        case MD_IMAGE: {
            char *path;
            if (strstr(b->src, "://")) path = g_strdup(b->src);
            else {
                char *e = path_expand_tilde(b->src);
                char *u = g_uri_unescape_string(e, NULL);
                if (u) { g_free(e); e = u; }
                if (e[0] != '/' && st->base_dir) { path = g_build_filename(st->base_dir, e, NULL); g_free(e); }
                else path = e;
            }
            el = el_image(path, b->alt);
            g_free(path);
            break;
        }
        }
        g_free(key);
        if (!el) continue;
        if (out->len) {
            double gap = 12;
            if (b->kind == MD_LIST && prev_heading) gap = 8;
            if (b->kind == MD_PARA && prev_heading) gap = 6;
            g_ptr_array_add(out, el_spacer(gap + top_extra));
        }
        g_ptr_array_add(out, el);
        prev_heading = b->kind == MD_HEADING;
    }
    g_ptr_array_unref(blocks);
    return el_vstack(out, 0);
}

El *md_element(const char *text, const MDStyle *st, const char *prefix) { return md_render(md_parse(text, st), st, prefix); }
