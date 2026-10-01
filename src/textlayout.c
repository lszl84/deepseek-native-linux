#include "textlayout.h"
#include "util.h"
#include <math.h>
#include <string.h>

RichText *rt_new(void) {
    RichText *rt = g_new0(RichText, 1);
    rt->text = g_string_new("");
    rt->spans = g_array_new(FALSE, FALSE, sizeof(RSpan));
    rt->links = g_ptr_array_new_with_free_func(g_free);
    return rt;
}

void rt_free(RichText *rt) {
    if (!rt) return;
    g_string_free(rt->text, TRUE);
    g_array_unref(rt->spans);
    g_ptr_array_unref(rt->links);
    g_free(rt);
}

static void append_span(RichText *rt, const char *s, gssize len, FontId f, int flags, ColorId c, double alpha, int link) {
    if (!s) return;
    if (len < 0) len = strlen(s);
    if (len == 0) return;
    int start = rt->text->len;
    if (g_utf8_validate(s, len, NULL)) g_string_append_len(rt->text, s, len);
    else {
        char *v = g_utf8_make_valid(s, len);
        g_string_append(rt->text, v);
        g_free(v);
    }
    RSpan sp = { start, (int)rt->text->len, f, flags, c, alpha, link };
    /* merge with the previous span when the style is identical */
    if (rt->spans->len) {
        RSpan *last = &g_array_index(rt->spans, RSpan, rt->spans->len - 1);
        if (last->end == start && last->font == f && last->flags == flags && last->color == c && last->alpha == alpha && last->link == link && !(flags & FS_CODE)) {
            last->end = sp.end;
            return;
        }
    }
    g_array_append_val(rt->spans, sp);
}

void rt_append(RichText *rt, const char *s, gssize len, FontId f, int flags, ColorId c) { append_span(rt, s, len, f, flags, c, 1, -1); }

void rt_append_alpha(RichText *rt, const char *s, gssize len, FontId f, int flags, ColorId c, double alpha) {
    append_span(rt, s, len, f, flags, c, alpha, -1);
}

void rt_append_link(RichText *rt, const char *s, gssize len, FontId f, int flags, ColorId c, const char *url) {
    g_ptr_array_add(rt->links, g_strdup(url));
    append_span(rt, s, len, f, flags, c, 1, rt->links->len - 1);
}

RichText *rt_plain(const char *s, FontId f, int flags, ColorId c) {
    RichText *rt = rt_new();
    rt_append(rt, s ? s : "", -1, f, flags, c);
    return rt;
}

TextLayout *tl_new(RichText *rt, int spacing) {
    TextLayout *t = g_new0(TextLayout, 1);
    t->rt = rt;
    t->spacing = spacing;
    t->align = PANGO_ALIGN_LEFT;
    t->laid_w = -1;
    for (guint i = 0; i < rt->spans->len; i++)
        if (g_array_index(rt->spans, RSpan, i).flags & FS_CODE) t->has_code = TRUE;
    return t;
}

void tl_free(TextLayout *t) {
    if (!t) return;
    rt_free(t->rt);
    if (t->layout) g_object_unref(t->layout);
    g_free(t);
}

int tl_length(TextLayout *t) { return t->rt->text->len; }
const char *tl_text(TextLayout *t) { return t->rt->text->str; }

static void apply_attrs(TextLayout *t) {
    PangoAttrList *al = pango_attr_list_new();
    RichText *rt = t->rt;
    for (guint i = 0; i < rt->spans->len; i++) {
        RSpan *s = &g_array_index(rt->spans, RSpan, i);
        PangoFontDescription *fd = theme_font_styled(s->font, s->flags);
        PangoAttribute *a = pango_attr_font_desc_new(fd);
        pango_font_description_free(fd);
        a->start_index = s->start;
        a->end_index = s->end;
        pango_attr_list_insert(al, a);
        RGBA c = th(s->color);
        a = pango_attr_foreground_new((guint16)(c.r * 65535), (guint16)(c.g * 65535), (guint16)(c.b * 65535));
        a->start_index = s->start;
        a->end_index = s->end;
        pango_attr_list_insert(al, a);
        double alpha = c.a * s->alpha;
        if (alpha < 0.999) {
            a = pango_attr_foreground_alpha_new((guint16)(alpha * 65535));
            a->start_index = s->start;
            a->end_index = s->end;
            pango_attr_list_insert(al, a);
        }
        if (s->flags & FS_STRIKE) {
            a = pango_attr_strikethrough_new(TRUE);
            a->start_index = s->start;
            a->end_index = s->end;
            pango_attr_list_insert(al, a);
        }
    }
    pango_layout_set_attributes(t->layout, al);
    pango_attr_list_unref(al);
    if (rt->spans->len) {
        RSpan *s0 = &g_array_index(rt->spans, RSpan, 0);
        PangoFontDescription *fd = theme_font_styled(s0->font, s0->flags & ~(FS_MONO | FS_CODE | FS_BOLD | FS_ITALIC));
        pango_layout_set_font_description(t->layout, fd);
        pango_font_description_free(fd);
    }
}

void tl_layout(TextLayout *t, double width) {
    guint gen = theme_generation();
    if (t->layout && fabs(width - t->laid_w) < 0.5 && gen == t->gen) return;
    if (!t->layout || gen != t->gen) {
        if (t->layout) g_object_unref(t->layout);
        t->layout = pango_layout_new(theme_pango());
        pango_layout_set_text(t->layout, t->rt->text->str, t->rt->text->len);
        apply_attrs(t);
        pango_layout_set_spacing(t->layout, t->spacing * PANGO_SCALE);
        pango_layout_set_alignment(t->layout, t->align);
        pango_layout_set_wrap(t->layout, t->char_wrap ? PANGO_WRAP_CHAR : PANGO_WRAP_WORD_CHAR);
        if (t->max_lines > 0) {
            pango_layout_set_height(t->layout, -t->max_lines);
            pango_layout_set_ellipsize(t->layout, PANGO_ELLIPSIZE_END);
        }
        t->gen = gen;
    }
    t->laid_w = width;
    pango_layout_set_width(t->layout, width > 0 ? (int)(MAX(10, width) * PANGO_SCALE) : -1);
    if (t->rt->text->len == 0) { t->w = 0; t->h = 0; return; }
    PangoRectangle log;
    pango_layout_get_pixel_extents(t->layout, NULL, &log);
    t->w = log.x + log.width;
    t->h = log.height;
    if (t->align != PANGO_ALIGN_LEFT) t->w = log.width;
}

static void draw_code_bgs(TextLayout *t, cairo_t *cr, double x, double y) {
    RichText *rt = t->rt;
    PangoLayoutIter *it = pango_layout_get_iter(t->layout);
    RGBA bg = th(C_INLINE_CODE_BG);
    do {
        PangoLayoutLine *line = pango_layout_iter_get_line_readonly(it);
        PangoRectangle lr;
        pango_layout_iter_get_line_extents(it, NULL, &lr);
        int ls = line->start_index, le = line->start_index + line->length;
        int baseline = pango_layout_iter_get_baseline(it);
        for (guint i = 0; i < rt->spans->len; i++) {
            RSpan *s = &g_array_index(rt->spans, RSpan, i);
            if (!(s->flags & FS_CODE)) continue;
            int a = MAX(s->start, ls), b = MIN(s->end, le);
            if (a >= b) continue;
            int *ranges = NULL, n = 0;
            pango_layout_line_get_x_ranges(line, a, b, &ranges, &n);
            for (int k = 0; k < n; k++) {
                double x1 = x + ranges[2 * k] / (double)PANGO_SCALE, x2 = x + ranges[2 * k + 1] / (double)PANGO_SCALE;
                double fh = theme_font_px(s->font) * 0.9;
                double by = y + baseline / (double)PANGO_SCALE;
                fill_rounded(cr, x1 - 3, by - fh * 0.92 - 2, x2 - x1 + 6, fh * 1.22 + 4, 5, bg);
            }
            g_free(ranges);
        }
        (void)lr;
    } while (pango_layout_iter_next_line(it));
    pango_layout_iter_free(it);
}

static void draw_selection(TextLayout *t, cairo_t *cr, double x, double y, int ss, int se) {
    PangoLayoutIter *it = pango_layout_get_iter(t->layout);
    set_color(cr, C_SELECTION);
    do {
        PangoLayoutLine *line = pango_layout_iter_get_line_readonly(it);
        PangoRectangle lr;
        pango_layout_iter_get_line_extents(it, NULL, &lr);
        int ls = line->start_index, le = line->start_index + line->length;
        int a = MAX(ss, ls), b = MIN(se, le);
        double top = y + lr.y / (double)PANGO_SCALE, h = lr.height / (double)PANGO_SCALE;
        gboolean past_end = se > le;
        if (a < b) {
            int *ranges = NULL, n = 0;
            pango_layout_line_get_x_ranges(line, a, b, &ranges, &n);
            for (int k = 0; k < n; k++) {
                double x1 = ranges[2 * k] / (double)PANGO_SCALE, x2 = ranges[2 * k + 1] / (double)PANGO_SCALE;
                cairo_rectangle(cr, x + x1, top, MAX(2, x2 - x1 + (past_end && k == n - 1 ? 4 : 0)), h);
            }
            g_free(ranges);
            cairo_fill(cr);
        } else if (ss <= ls && se > le && line->length == 0) {
            cairo_rectangle(cr, x + lr.x / (double)PANGO_SCALE, top, 4, h);
            cairo_fill(cr);
        }
    } while (pango_layout_iter_next_line(it));
    pango_layout_iter_free(it);
}

void tl_draw(TextLayout *t, cairo_t *cr, double x, double y, double clip_top, double clip_bottom, int ss, int se) {
    if (!t->layout || t->rt->text->len == 0) return;
    if (y > clip_bottom || y + t->h < clip_top) return;
    if (ss >= 0 && se > ss) draw_selection(t, cr, x, y, ss, se);
    if (t->has_code) draw_code_bgs(t, cr, x, y);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, t->layout);
}

int tl_index_at(TextLayout *t, double x, double y) {
    if (!t->layout || !t->rt->text->len) return 0;
    if (y < 0) return 0;
    if (y > t->h) return t->rt->text->len;
    int idx = 0, trailing = 0;
    pango_layout_xy_to_index(t->layout, (int)(x * PANGO_SCALE), (int)(y * PANGO_SCALE), &idx, &trailing);
    const char *s = t->rt->text->str;
    const char *p = s + idx;
    for (int i = 0; i < trailing && *p; i++) p = g_utf8_next_char(p);
    return p - s;
}

const char *tl_link_at(TextLayout *t, double x, double y) {
    if (!t->layout || !t->rt->links->len) return NULL;
    int idx = 0, trailing = 0;
    if (!pango_layout_xy_to_index(t->layout, (int)(x * PANGO_SCALE), (int)(y * PANGO_SCALE), &idx, &trailing)) return NULL;
    for (guint i = 0; i < t->rt->spans->len; i++) {
        RSpan *s = &g_array_index(t->rt->spans, RSpan, i);
        if (s->link >= 0 && idx >= s->start && idx < s->end) return g_ptr_array_index(t->rt->links, s->link);
    }
    return NULL;
}

gboolean tl_contains(TextLayout *t, double x, double y) {
    if (!t->layout || !t->rt->text->len) return FALSE;
    if (y < -2 || y > t->h + 2) return FALSE;
    int idx, trailing;
    return pango_layout_xy_to_index(t->layout, (int)(x * PANGO_SCALE), (int)(y * PANGO_SCALE), &idx, &trailing);
}

static gboolean is_word(gunichar c) { return g_unichar_isalnum(c) || c == '_'; }

void tl_word_range(TextLayout *t, int idx, int *s, int *e) {
    const char *str = t->rt->text->str;
    int len = t->rt->text->len;
    if (!len) { *s = *e = 0; return; }
    if (idx >= len) idx = len - 1;
    if (idx < 0) idx = 0;
    const char *p = g_utf8_find_prev_char(str, str + idx + 1);
    if (!p) p = str;
    if (!is_word(g_utf8_get_char(p))) {
        *s = p - str;
        *e = g_utf8_next_char(p) - str;
        return;
    }
    const char *a = p, *b = p;
    for (;;) {
        const char *q = g_utf8_find_prev_char(str, a);
        if (!q || !is_word(g_utf8_get_char(q))) break;
        a = q;
    }
    while (*b && is_word(g_utf8_get_char(b))) b = g_utf8_next_char(b);
    *s = a - str;
    *e = b - str;
}

void tl_line_range(TextLayout *t, int idx, int *s, int *e) {
    const char *str = t->rt->text->str;
    int len = t->rt->text->len;
    if (idx > len) idx = len;
    int a = idx, b = idx;
    while (a > 0 && str[a - 1] != '\n') a--;
    while (b < len && str[b] != '\n') b++;
    if (b < len) b++;
    *s = a;
    *e = b;
}

double tl_longest_word(TextLayout *t) {
    const char *s = t->rt->text->str;
    double best = 0;
    int start = 0, len = t->rt->text->len;
    FontId f = t->rt->spans->len ? g_array_index(t->rt->spans, RSpan, 0).font : F_BODY;
    for (int i = 0; i <= len; i++) {
        if (i == len || s[i] == ' ' || s[i] == '\n') {
            if (i > start) {
                char *w = g_strndup(s + start, i - start);
                best = MAX(best, text_width(w, f, 0));
                g_free(w);
            }
            start = i + 1;
        }
    }
    return ceil(best);
}
