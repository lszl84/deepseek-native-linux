#include "transcript.h"
#include "util.h"
#include <math.h>
#include <string.h>

typedef struct {
    char *key;
    Sig sig;
    double gap;
    El *el;
    double y, h;
    double laid_w;
    guint idx; /* position in DsTranscript.items */
} Item;

struct _DsTranscript {
    GtkDrawingArea parent;
    GtkAdjustment *hadj, *vadj;
    guint hpolicy, vpolicy;
    GPtrArray *items;    /* Item* */
    GHashTable *by_key;  /* key -> Item* */
    DrawEnv env;
    double max_col, side_pad, top_inset, bottom_inset, scrim;
    double content_h;
    /* selection */
    GArray *refs; /* TextRef */
    GHashTable *ref_index;
    gboolean refs_dirty;
    TextLayout *anchor_tl, *focus_tl;
    int anchor_i, focus_i;
    gboolean selecting;
    HitResult down_hit;
    gboolean has_down_hit;
    double down_x, down_y;
    double last_x, last_y;
    guint anim_timer, copied_timer, autoscroll_timer;
    TranscriptHitFn hit_fn;
    gpointer hit_data;
    TranscriptTypeFn type_fn;
    gpointer type_data;
    void (*scroll_fn)(gpointer);
    gpointer scroll_data;
    char *hover_key_item;
    const char *cursor_name;
};

enum { PROP_0, PROP_HADJ, PROP_VADJ, PROP_HPOLICY, PROP_VPOLICY };

G_DEFINE_TYPE_WITH_CODE(DsTranscript, ds_transcript, GTK_TYPE_DRAWING_AREA, G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, NULL))

static void item_free(gpointer p) {
    Item *it = p;
    if (!it) return;
    g_free(it->key);
    el_free(it->el);
    g_free(it);
}

static double col_width(DsTranscript *t) {
    double w = gtk_widget_get_allocated_width(GTK_WIDGET(t));
    return MAX(200, MIN(t->max_col, w - t->side_pad * 2));
}

static double col_x(DsTranscript *t) {
    double w = gtk_widget_get_allocated_width(GTK_WIDGET(t));
    return floor((w - col_width(t)) / 2);
}

static double offset(DsTranscript *t) { return t->vadj ? gtk_adjustment_get_value(t->vadj) : 0; }

static void update_adjustment(DsTranscript *t) {
    if (!t->vadj) return;
    double h = gtk_widget_get_allocated_height(GTK_WIDGET(t));
    double upper = MAX(t->content_h, h);
    double v = CLAMP(gtk_adjustment_get_value(t->vadj), 0, MAX(0, upper - h));
    gtk_adjustment_configure(t->vadj, v, 0, upper, 40, h * 0.9, h);
}

static void relayout_positions(DsTranscript *t) {
    double y = t->top_inset;
    for (guint i = 0; i < t->items->len; i++) {
        Item *it = g_ptr_array_index(t->items, i);
        if (i > 0) y += it->gap;
        it->y = y;
        y += it->h;
    }
    double h = y + t->bottom_inset;
    if (fabs(h - t->content_h) > 0.5) {
        t->content_h = h;
        update_adjustment(t);
    }
}

static void layout_items(DsTranscript *t, gboolean all) {
    double w = col_width(t);
    for (guint i = 0; i < t->items->len; i++) {
        Item *it = g_ptr_array_index(t->items, i);
        if (all || fabs(it->laid_w - w) > 0.5) {
            el_layout(it->el, w);
            it->h = it->el->h;
            it->laid_w = w;
        }
    }
}

static void queue_doc_rect(DsTranscript *t, double y, double h) {
    double off = offset(t);
    int wh = gtk_widget_get_allocated_height(GTK_WIDGET(t)), ww = gtk_widget_get_allocated_width(GTK_WIDGET(t));
    double top = y - off, bottom = top + h;
    if (bottom < 0 || top > wh) return;
    top = MAX(0, top);
    bottom = MIN(wh, bottom);
    gtk_widget_queue_draw_area(GTK_WIDGET(t), 0, (int)floor(top), ww, (int)ceil(bottom - top) + 1);
}

static void queue_item(DsTranscript *t, Item *it) { queue_doc_rect(t, it->y - 6, it->h + 12); }

static int first_visible(DsTranscript *t, double min_y) {
    int lo = 0, hi = t->items->len;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        Item *it = g_ptr_array_index(t->items, mid);
        if (it->y + it->h < min_y) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static int item_at(DsTranscript *t, double y) {
    int lo = 0, hi = (int)t->items->len - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        Item *it = g_ptr_array_index(t->items, mid);
        if (y < it->y) hi = mid - 1;
        else if (y > it->y + it->h) lo = mid + 1;
        else return mid;
    }
    return -1;
}

/* ---------------- animation ---------------- */

static gboolean anim_tick(gpointer p) {
    DsTranscript *t = p;
    t->env.phase = fmod(g_get_monotonic_time() / 1e6 * 1.1, 1.0);
    double off = offset(t), h = gtk_widget_get_allocated_height(GTK_WIDGET(t));
    gboolean any = FALSE;
    for (int i = first_visible(t, off); i < (int)t->items->len; i++) {
        Item *it = g_ptr_array_index(t->items, i);
        if (it->y > off + h) break;
        if (el_animating(it->el)) { queue_item(t, it); any = TRUE; }
    }
    gboolean needed = FALSE;
    for (guint i = 0; i < t->items->len && !needed; i++)
        if (el_animating(((Item *)g_ptr_array_index(t->items, i))->el)) needed = TRUE;
    (void)any;
    if (!needed) { t->anim_timer = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

static void update_anim(DsTranscript *t) {
    gboolean needed = FALSE;
    for (guint i = 0; i < t->items->len && !needed; i++)
        if (el_animating(((Item *)g_ptr_array_index(t->items, i))->el)) needed = TRUE;
    if (needed && !t->anim_timer) t->anim_timer = g_timeout_add(83, anim_tick, t);
}

/* ---------------- selection ---------------- */

static void ensure_refs(DsTranscript *t) {
    if (!t->refs_dirty) return;
    t->refs_dirty = FALSE;
    g_array_set_size(t->refs, 0);
    g_hash_table_remove_all(t->ref_index);
    double x = col_x(t);
    for (guint i = 0; i < t->items->len; i++) {
        Item *it = g_ptr_array_index(t->items, i);
        el_texts(it->el, x, it->y, t->refs);
    }
    for (guint i = 0; i < t->refs->len; i++) g_hash_table_insert(t->ref_index, g_array_index(t->refs, TextRef, i).tl, GINT_TO_POINTER(i + 1));
}

static int ref_of(DsTranscript *t, TextLayout *tl) { return GPOINTER_TO_INT(g_hash_table_lookup(t->ref_index, tl)) - 1; }

static gboolean ordered(DsTranscript *t, int *si, int *sx, int *ei, int *ex) {
    if (!t->anchor_tl || !t->focus_tl) return FALSE;
    int ai = ref_of(t, t->anchor_tl), fi = ref_of(t, t->focus_tl);
    if (ai < 0 || fi < 0) return FALSE;
    if (ai < fi || (ai == fi && t->anchor_i <= t->focus_i)) { *si = ai; *sx = t->anchor_i; *ei = fi; *ex = t->focus_i; }
    else { *si = fi; *sx = t->focus_i; *ei = ai; *ex = t->anchor_i; }
    return TRUE;
}

static void update_selection_map(DsTranscript *t) {
    g_hash_table_remove_all(t->env.selection);
    int si, sx, ei, ex;
    if (ordered(t, &si, &sx, &ei, &ex)) {
        for (int i = si; i <= ei; i++) {
            TextLayout *tl = g_array_index(t->refs, TextRef, i).tl;
            int start = i == si ? sx : 0, end = i == ei ? ex : tl_length(tl);
            if (end > start) g_hash_table_insert(t->env.selection, tl, (gpointer)(gintptr)(((gint64)start << 32) | (gint64)end));
        }
    }
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

void ds_transcript_clear_selection(DsTranscript *t) {
    t->anchor_tl = t->focus_tl = NULL;
    if (g_hash_table_size(t->env.selection)) {
        g_hash_table_remove_all(t->env.selection);
        gtk_widget_queue_draw(GTK_WIDGET(t));
    }
}

static void validate_selection(DsTranscript *t) {
    if (!t->anchor_tl) return;
    ensure_refs(t);
    int a, b, c, d;
    if (!ordered(t, &a, &b, &c, &d)) ds_transcript_clear_selection(t);
    else update_selection_map(t);
}

/* Nearest (ref, index) for a point in document coordinates. */
static gboolean sel_point(DsTranscript *t, double x, double y, int *ref, int *idx) {
    ensure_refs(t);
    if (!t->refs->len) return FALSE;
    int best = -1;
    double best_dx = G_MAXDOUBLE;
    for (guint i = 0; i < t->refs->len; i++) {
        TextRef *r = &g_array_index(t->refs, TextRef, i);
        double top = r->y - 3, bottom = r->y + r->tl->h + 3;
        if (y >= top && y <= bottom) {
            double right = r->x + MAX(r->tl->laid_w, r->tl->w);
            double dx = x < r->x ? r->x - x : x > right ? x - right : 0;
            if (dx < best_dx) { best_dx = dx; best = i; }
        } else if (r->y > bottom + 2000) break;
    }
    if (best >= 0) {
        TextRef *r = &g_array_index(t->refs, TextRef, best);
        *ref = best;
        *idx = tl_index_at(r->tl, x - r->x, y - r->y);
        return TRUE;
    }
    for (guint i = 0; i < t->refs->len; i++) {
        TextRef *r = &g_array_index(t->refs, TextRef, i);
        if (r->y > y) {
            if (i > 0) { *ref = i - 1; *idx = tl_length(g_array_index(t->refs, TextRef, i - 1).tl); }
            else { *ref = 0; *idx = 0; }
            return TRUE;
        }
    }
    *ref = t->refs->len - 1;
    *idx = tl_length(g_array_index(t->refs, TextRef, *ref).tl);
    return TRUE;
}

char *ds_transcript_selected_text(DsTranscript *t) {
    ensure_refs(t);
    int si, sx, ei, ex;
    if (!ordered(t, &si, &sx, &ei, &ex)) return g_strdup("");
    GString *s = g_string_new("");
    for (int i = si; i <= ei; i++) {
        TextLayout *tl = g_array_index(t->refs, TextRef, i).tl;
        int start = i == si ? sx : 0, end = i == ei ? ex : tl_length(tl);
        if (end > start) {
            if (s->len) g_string_append_c(s, '\n');
            g_string_append_len(s, tl_text(tl) + start, end - start);
        }
    }
    char *a = str_replace(s->str, " ", "");
    char *b = str_replace(a, " ", "\n");
    g_free(a);
    g_string_free(s, TRUE);
    return b;
}

static void select_all(DsTranscript *t) {
    ensure_refs(t);
    if (!t->refs->len) return;
    t->anchor_tl = g_array_index(t->refs, TextRef, 0).tl;
    t->anchor_i = 0;
    t->focus_tl = g_array_index(t->refs, TextRef, t->refs->len - 1).tl;
    t->focus_i = tl_length(t->focus_tl);
    update_selection_map(t);
}

static void copy_text(DsTranscript *t, const char *text, GdkAtom which) {
    GtkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(t), which);
    gtk_clipboard_set_text(cb, text, -1);
}

static void copy_selection(DsTranscript *t) {
    char *s = ds_transcript_selected_text(t);
    if (*s) copy_text(t, s, GDK_SELECTION_CLIPBOARD);
    g_free(s);
}

/* ---------------- apply ---------------- */

gboolean ds_transcript_have(const char *key, Sig sig, gpointer self) {
    DsTranscript *t = self;
    Item *it = g_hash_table_lookup(t->by_key, key);
    return it && it->sig == sig;
}

void ds_transcript_apply(DsTranscript *t, GPtrArray *specs) {
    GPtrArray *items = g_ptr_array_new_with_free_func(item_free);
    GHashTable *by_key = g_hash_table_new(g_str_hash, g_str_equal);
    double w = col_width(t);
    gboolean changed = FALSE;
    double moved_from = G_MAXDOUBLE;
    for (guint i = 0; i < specs->len; i++) {
        ItemSpec *s = g_ptr_array_index(specs, i);
        Item *old = g_hash_table_lookup(t->by_key, s->key);
        if (old && !g_hash_table_contains(by_key, s->key) && (old->sig == s->sig || s->el)) {
            g_hash_table_steal(t->by_key, s->key);
            /* take it out of the old list without freeing */
            if (old->idx < t->items->len && t->items->pdata[old->idx] == old) t->items->pdata[old->idx] = NULL;
            if (s->el && old->sig != s->sig) {
                el_free(old->el);
                old->el = s->el;
                s->el = NULL;
                old->sig = s->sig;
                old->laid_w = -1;
                changed = TRUE;
                moved_from = MIN(moved_from, old->y - 8);
            } else if (s->el) {
                el_free(s->el);
                s->el = NULL;
            }
            if (fabs(old->gap - s->gap) > 0.1) { old->gap = s->gap; changed = TRUE; moved_from = MIN(moved_from, old->y - 30); }
            g_ptr_array_add(items, old);
            g_hash_table_insert(by_key, old->key, old);
        } else {
            Item *it = g_new0(Item, 1);
            it->key = g_hash_table_contains(by_key, s->key) ? g_strdup_printf("%s#%u", s->key, items->len) : g_strdup(s->key);
            it->sig = s->sig;
            it->gap = s->gap;
            it->el = s->el ? s->el : el_spacer(0);
            s->el = NULL;
            it->laid_w = -1;
            it->y = G_MAXDOUBLE / 4;
            g_ptr_array_add(items, it);
            g_hash_table_insert(by_key, it->key, it);
            changed = TRUE;
        }
    }
    g_ptr_array_unref(specs);
    /* items left in the old list are removed */
    gboolean removed = FALSE;
    for (guint k = 0; k < t->items->len; k++) {
        Item *it = g_ptr_array_index(t->items, k);
        if (it) { removed = TRUE; moved_from = MIN(moved_from, it->y - 8); }
    }
    if (!changed && !removed && items->len == t->items->len) {
        /* identical list */
        for (guint k = 0; k < t->items->len; k++) t->items->pdata[k] = NULL;
        g_ptr_array_unref(t->items);
        g_hash_table_unref(t->by_key);
        t->items = items;
        t->by_key = by_key;
        for (guint i = 0; i < t->items->len; i++) ((Item *)g_ptr_array_index(t->items, i))->idx = i;
        return;
    }
    g_ptr_array_unref(t->items); /* frees removed items only (others were nulled) */
    g_hash_table_unref(t->by_key);
    t->items = items;
    t->by_key = by_key;
    for (guint i = 0; i < t->items->len; i++) ((Item *)g_ptr_array_index(t->items, i))->idx = i;
    double old_h = t->content_h;
    guint n = t->items->len;
    double *prev_y = g_new(double, MAX(1, n));
    for (guint i = 0; i < n; i++) prev_y[i] = ((Item *)g_ptr_array_index(t->items, i))->y;
    for (guint i = 0; i < n; i++) {
        Item *it = g_ptr_array_index(t->items, i);
        if (fabs(it->laid_w - w) > 0.5) {
            double oh = it->h;
            el_layout(it->el, w);
            it->h = it->el->h;
            it->laid_w = w;
            if (fabs(oh - it->h) > 0.5) moved_from = MIN(moved_from, prev_y[i] < G_MAXDOUBLE / 8 ? prev_y[i] : moved_from);
        }
    }
    relayout_positions(t);
    for (guint i = 0; i < n; i++) {
        Item *it = g_ptr_array_index(t->items, i);
        if (prev_y[i] >= G_MAXDOUBLE / 8) { moved_from = MIN(moved_from, it->y - 8); continue; }
        if (fabs(prev_y[i] - it->y) > 0.5) moved_from = MIN(moved_from, MIN(prev_y[i], it->y) - 8);
    }
    g_free(prev_y);
    t->refs_dirty = TRUE;
    validate_selection(t);
    if (moved_from < G_MAXDOUBLE) queue_doc_rect(t, moved_from, MAX(old_h, t->content_h) - moved_from + 8);
    update_anim(t);
}

void ds_transcript_clear(DsTranscript *t) { ds_transcript_apply(t, g_ptr_array_new_with_free_func(item_spec_free)); }

void ds_transcript_invalidate_item(DsTranscript *t, const char *key) {
    Item *it = g_hash_table_lookup(t->by_key, key);
    if (it) queue_item(t, it);
}

void ds_transcript_relayout_all(DsTranscript *t) {
    layout_items(t, TRUE);
    relayout_positions(t);
    t->refs_dirty = TRUE;
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

GPtrArray *ds_transcript_keys(DsTranscript *t) {
    GPtrArray *a = g_ptr_array_new();
    for (guint i = 0; i < t->items->len; i++) g_ptr_array_add(a, ((Item *)g_ptr_array_index(t->items, i))->key);
    return a;
}

/* ---------------- drawing ---------------- */

static gboolean ds_draw(GtkWidget *w, cairo_t *cr) {
    gint64 perf_t0 = g_get_monotonic_time();
    DsTranscript *t = DS_TRANSCRIPT(w);
    double width = gtk_widget_get_allocated_width(w), height = gtk_widget_get_allocated_height(w);
    double off = offset(t);
    set_color(cr, C_BG);
    cairo_paint(cr);
    double x1, y1, x2, y2;
    cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
    t->env.clip_top = y1 + off - 8;
    t->env.clip_bottom = y2 + off + 8;
    t->env.scale = gtk_widget_get_scale_factor(w);
    cairo_save(cr);
    cairo_translate(cr, 0, -off);
    double x = col_x(t);
    for (int i = first_visible(t, t->env.clip_top); i < (int)t->items->len; i++) {
        Item *it = g_ptr_array_index(t->items, i);
        if (it->y > t->env.clip_bottom) break;
        el_draw(it->el, cr, x, it->y, &t->env);
    }
    cairo_restore(cr);
    if (t->scrim > 0) {
        double top = height - t->scrim;
        RGBA bg = th(C_BG);
        cairo_pattern_t *p = cairo_pattern_create_linear(0, top, 0, height);
        cairo_pattern_add_color_stop_rgba(p, 0, bg.r, bg.g, bg.b, 0);
        cairo_pattern_add_color_stop_rgba(p, 0.45, bg.r, bg.g, bg.b, 1);
        cairo_pattern_add_color_stop_rgba(p, 1, bg.r, bg.g, bg.b, 1);
        cairo_set_source(cr, p);
        cairo_rectangle(cr, 0, top, width - 12, t->scrim);
        cairo_fill(cr);
        cairo_pattern_destroy(p);
    }
    perf_note("draw", g_get_monotonic_time() - perf_t0);
    return FALSE;
}

/* ---------------- events ---------------- */

static gboolean hit_test(DsTranscript *t, double x, double y, HitResult *r, char **item_key) {
    int i = item_at(t, y);
    if (i < 0) return FALSE;
    Item *it = g_ptr_array_index(t->items, i);
    if (item_key) *item_key = it->key;
    return el_hit(it->el, x - col_x(t), y - it->y, r);
}

static gboolean over_text(DsTranscript *t, double x, double y) {
    ensure_refs(t);
    for (guint i = 0; i < t->refs->len; i++) {
        TextRef *r = &g_array_index(t->refs, TextRef, i);
        if (y >= r->y && y <= r->y + r->tl->h && tl_contains(r->tl, x - r->x, y - r->y)) return TRUE;
        if (r->y > y + 2000) break;
    }
    return FALSE;
}

static void set_cursor(DsTranscript *t, const char *name) {
    if (t->cursor_name == name) return;
    t->cursor_name = name;
    GdkWindow *win = gtk_widget_get_window(GTK_WIDGET(t));
    if (!win) return;
    GdkCursor *c = name ? gdk_cursor_new_from_name(gdk_window_get_display(win), name) : NULL;
    gdk_window_set_cursor(win, c);
    if (c) g_object_unref(c);
}

static void set_hover(DsTranscript *t, const char *key, const char *key_item, const char *item) {
    if (!g_strcmp0(key, t->env.hover_key) && !g_strcmp0(item, t->env.hover_item)) return;
    const char *keys[] = { t->env.hover_item, item, t->hover_key_item, key_item };
    for (int i = 0; i < 4; i++) {
        if (!keys[i]) continue;
        Item *it = g_hash_table_lookup(t->by_key, keys[i]);
        if (it) queue_item(t, it);
    }
    g_free(t->env.hover_key);
    t->env.hover_key = g_strdup(key);
    g_free(t->env.hover_item);
    t->env.hover_item = g_strdup(item);
    g_free(t->hover_key_item);
    t->hover_key_item = g_strdup(key_item);
}

static void update_hover(DsTranscript *t, double x, double y) {
    double dy = y + offset(t);
    HitResult r = { 0 };
    char *hit_item = NULL;
    gboolean hit = hit_test(t, x, dy, &r, &hit_item);
    int ii = item_at(t, dy);
    const char *item = ii >= 0 ? ((Item *)g_ptr_array_index(t->items, ii))->key : NULL;
    set_hover(t, hit ? r.key : NULL, hit ? hit_item : NULL, item);
    if (hit) set_cursor(t, "pointer");
    else if (over_text(t, x, dy)) set_cursor(t, "text");
    else set_cursor(t, NULL);
    hit_result_clear(&r);
}

static gboolean clear_copied(gpointer p) {
    DsTranscript *t = p;
    g_clear_pointer(&t->env.copied_key, g_free);
    t->copied_timer = 0;
    gtk_widget_queue_draw(GTK_WIDGET(t));
    return G_SOURCE_REMOVE;
}

static void flash_copied(DsTranscript *t, const char *key) {
    g_free(t->env.copied_key);
    t->env.copied_key = g_strdup(key);
    gtk_widget_queue_draw(GTK_WIDGET(t));
    if (t->copied_timer) g_source_remove(t->copied_timer);
    t->copied_timer = g_timeout_add(1400, clear_copied, t);
}

static void popup_menu(DsTranscript *t, GdkEventButton *ev);

static gboolean ds_press(GtkWidget *w, GdkEventButton *ev) {
    DsTranscript *t = DS_TRANSCRIPT(w);
    double x = ev->x, y = ev->y + offset(t);
    if (ev->button == 3 && ev->type == GDK_BUTTON_PRESS) { popup_menu(t, ev); return TRUE; }
    if (ev->button != 1) return FALSE;
    if (ev->type == GDK_BUTTON_PRESS) {
        t->down_x = x;
        t->down_y = y;
        HitResult r = { 0 };
        if (hit_test(t, x, y, &r, NULL)) {
            hit_result_clear(&t->down_hit);
            t->down_hit = r;
            t->has_down_hit = TRUE;
            g_free(t->env.pressed_key);
            t->env.pressed_key = g_strdup(r.key);
            return TRUE;
        }
        t->has_down_hit = FALSE;
    } else if (t->has_down_hit) {
        return TRUE; /* double-clicks on buttons are not selections */
    }
    int ref, idx;
    if (!sel_point(t, x, y, &ref, &idx)) { ds_transcript_clear_selection(t); return TRUE; }
    gtk_widget_grab_focus(w);
    TextLayout *tl = g_array_index(t->refs, TextRef, ref).tl;
    if (ev->type == GDK_2BUTTON_PRESS) {
        int s, e;
        tl_word_range(tl, idx, &s, &e);
        t->anchor_tl = t->focus_tl = tl;
        t->anchor_i = s;
        t->focus_i = e;
    } else if (ev->type == GDK_3BUTTON_PRESS) {
        int s, e;
        tl_line_range(tl, idx, &s, &e);
        t->anchor_tl = t->focus_tl = tl;
        t->anchor_i = s;
        t->focus_i = e;
    } else if ((ev->state & GDK_SHIFT_MASK) && t->anchor_tl) {
        t->focus_tl = tl;
        t->focus_i = idx;
    } else {
        t->anchor_tl = t->focus_tl = tl;
        t->anchor_i = t->focus_i = idx;
    }
    t->selecting = TRUE;
    update_selection_map(t);
    return TRUE;
}

static void drag_to(DsTranscript *t, double x, double wy) {
    int ref, idx;
    if (sel_point(t, x, wy + offset(t), &ref, &idx)) {
        t->focus_tl = g_array_index(t->refs, TextRef, ref).tl;
        t->focus_i = idx;
        update_selection_map(t);
    }
}

static gboolean autoscroll_tick(gpointer p) {
    DsTranscript *t = p;
    double h = gtk_widget_get_allocated_height(GTK_WIDGET(t));
    double d = 0;
    if (t->last_y < 0) d = t->last_y;
    else if (t->last_y > h) d = t->last_y - h;
    if (!t->selecting || d == 0 || !t->vadj) { t->autoscroll_timer = 0; return G_SOURCE_REMOVE; }
    double v = gtk_adjustment_get_value(t->vadj) + CLAMP(d, -60, 60) * 0.5;
    gtk_adjustment_set_value(t->vadj, CLAMP(v, 0, gtk_adjustment_get_upper(t->vadj) - h));
    drag_to(t, t->last_x, t->last_y);
    return G_SOURCE_CONTINUE;
}

static gboolean ds_motion(GtkWidget *w, GdkEventMotion *ev) {
    DsTranscript *t = DS_TRANSCRIPT(w);
    t->last_x = ev->x;
    t->last_y = ev->y;
    if (ev->state & GDK_BUTTON1_MASK) {
        double y = ev->y + offset(t);
        if (t->has_down_hit) {
            if (hypot(ev->x - t->down_x, y - t->down_y) <= 4) return TRUE;
            t->has_down_hit = FALSE;
            hit_result_clear(&t->down_hit);
            g_clear_pointer(&t->env.pressed_key, g_free);
            int ref, idx;
            if (sel_point(t, t->down_x, t->down_y, &ref, &idx)) {
                t->anchor_tl = g_array_index(t->refs, TextRef, ref).tl;
                t->anchor_i = idx;
                t->selecting = TRUE;
                gtk_widget_grab_focus(w);
            }
        }
        if (!t->selecting) return TRUE;
        drag_to(t, ev->x, ev->y);
        double h = gtk_widget_get_allocated_height(w);
        if ((ev->y < 0 || ev->y > h) && !t->autoscroll_timer) t->autoscroll_timer = g_timeout_add(30, autoscroll_tick, t);
        return TRUE;
    }
    update_hover(t, ev->x, ev->y);
    return FALSE;
}

static gboolean ds_release(GtkWidget *w, GdkEventButton *ev) {
    DsTranscript *t = DS_TRANSCRIPT(w);
    if (ev->button != 1) return FALSE;
    g_clear_pointer(&t->env.pressed_key, g_free);
    if (t->has_down_hit) {
        t->has_down_hit = FALSE;
        HitResult r = { 0 };
        double y = ev->y + offset(t);
        if (hit_test(t, ev->x, y, &r, NULL) && !g_strcmp0(r.key, t->down_hit.key)) {
            Hit *h = &t->down_hit.hit;
            if (h->kind == HIT_COPY) {
                copy_text(t, h->a ? h->a : "", GDK_SELECTION_CLIPBOARD);
                flash_copied(t, h->b ? h->b : t->down_hit.key);
            } else if (t->hit_fn) {
                Hit copy = hit_copy(h);
                t->hit_fn(&copy, t->hit_data);
                hit_free_contents(&copy);
            }
        }
        hit_result_clear(&r);
        hit_result_clear(&t->down_hit);
        return TRUE;
    }
    t->selecting = FALSE;
    if (t->anchor_tl && t->anchor_tl == t->focus_tl && t->anchor_i == t->focus_i) ds_transcript_clear_selection(t);
    else if (t->anchor_tl) {
        char *s = ds_transcript_selected_text(t);
        if (*s) copy_text(t, s, GDK_SELECTION_PRIMARY);
        g_free(s);
    }
    return TRUE;
}

static gboolean ds_leave(GtkWidget *w, GdkEventCrossing *ev) {
    DsTranscript *t = DS_TRANSCRIPT(w);
    if (ev->mode != GDK_CROSSING_NORMAL) return FALSE;
    set_hover(t, NULL, NULL, NULL);
    set_cursor(t, NULL);
    return FALSE;
}

static gboolean ds_key(GtkWidget *w, GdkEventKey *ev) {
    DsTranscript *t = DS_TRANSCRIPT(w);
    gboolean ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
    if (ctrl && (ev->keyval == GDK_KEY_c || ev->keyval == GDK_KEY_C || ev->keyval == GDK_KEY_Insert)) { copy_selection(t); return TRUE; }
    if (ctrl && (ev->keyval == GDK_KEY_a || ev->keyval == GDK_KEY_A)) { select_all(t); return TRUE; }
    if (ev->keyval == GDK_KEY_Escape) { ds_transcript_clear_selection(t); return FALSE; }
    if (t->vadj && !ctrl) {
        double h = gtk_widget_get_allocated_height(w), v = gtk_adjustment_get_value(t->vadj), d = 0;
        switch (ev->keyval) {
        case GDK_KEY_Page_Down: d = h * 0.9; break;
        case GDK_KEY_Page_Up: d = -h * 0.9; break;
        case GDK_KEY_Down: d = 40; break;
        case GDK_KEY_Up: d = -40; break;
        case GDK_KEY_Home: d = -v; break;
        case GDK_KEY_End: d = t->content_h; break;
        default: break;
        }
        if (d != 0) {
            gtk_adjustment_set_value(t->vadj, CLAMP(v + d, 0, MAX(0, gtk_adjustment_get_upper(t->vadj) - h)));
            return TRUE;
        }
    }
    if (t->type_fn && !ctrl && !(ev->state & GDK_MOD1_MASK) && ev->length > 0 && g_unichar_isprint(gdk_keyval_to_unicode(ev->keyval))) {
        t->type_fn(ev, t->type_data);
        return TRUE;
    }
    return FALSE;
}

/* ---------------- context menu ---------------- */

static void menu_copy(GtkMenuItem *mi, gpointer p) { copy_selection(p); }
static void menu_select_all(GtkMenuItem *mi, gpointer p) { select_all(p); }

static void menu_open_link(GtkMenuItem *mi, gpointer p) {
    DsTranscript *t = p;
    const char *url = g_object_get_data(G_OBJECT(mi), "url");
    if (t->hit_fn && url) {
        Hit h = hit_make(HIT_LINK, url, NULL, NULL, 0);
        t->hit_fn(&h, t->hit_data);
        hit_free_contents(&h);
    }
}

static void menu_copy_link(GtkMenuItem *mi, gpointer p) {
    const char *url = g_object_get_data(G_OBJECT(mi), "url");
    if (url) copy_text(p, g_str_has_prefix(url, "file://") ? url + 7 : url, GDK_SELECTION_CLIPBOARD);
}

static void popup_menu(DsTranscript *t, GdkEventButton *ev) {
    GtkWidget *menu = gtk_menu_new();
    HitResult r = { 0 };
    if (hit_test(t, ev->x, ev->y + offset(t), &r, NULL) && r.hit.kind == HIT_LINK) {
        GtkWidget *open = gtk_menu_item_new_with_label("Open Link");
        g_object_set_data_full(G_OBJECT(open), "url", g_strdup(r.hit.a), g_free);
        g_signal_connect(open, "activate", G_CALLBACK(menu_open_link), t);
        GtkWidget *cp = gtk_menu_item_new_with_label("Copy Link");
        g_object_set_data_full(G_OBJECT(cp), "url", g_strdup(r.hit.a), g_free);
        g_signal_connect(cp, "activate", G_CALLBACK(menu_copy_link), t);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), open);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), cp);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    }
    hit_result_clear(&r);
    GtkWidget *copy = gtk_menu_item_new_with_mnemonic("_Copy");
    gtk_widget_set_sensitive(copy, g_hash_table_size(t->env.selection) > 0);
    g_signal_connect(copy, "activate", G_CALLBACK(menu_copy), t);
    GtkWidget *all = gtk_menu_item_new_with_mnemonic("Select _All");
    g_signal_connect(all, "activate", G_CALLBACK(menu_select_all), t);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), copy);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), all);
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(t), NULL);
    g_signal_connect(menu, "deactivate", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

/* ---------------- size / scrolling ---------------- */

static void vadj_changed(GtkAdjustment *a, gpointer p) {
    DsTranscript *t = p;
    gtk_widget_queue_draw(GTK_WIDGET(t));
    if (t->scroll_fn) t->scroll_fn(t->scroll_data);
}

static void set_vadj(DsTranscript *t, GtkAdjustment *a) {
    if (t->vadj == a) return;
    if (t->vadj) {
        g_signal_handlers_disconnect_by_data(t->vadj, t);
        g_object_unref(t->vadj);
    }
    if (!a) a = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
    t->vadj = g_object_ref_sink(a);
    g_signal_connect(a, "value-changed", G_CALLBACK(vadj_changed), t);
    update_adjustment(t);
}

static void set_hadj(DsTranscript *t, GtkAdjustment *a) {
    if (t->hadj) g_object_unref(t->hadj);
    if (!a) a = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
    t->hadj = g_object_ref_sink(a);
}

static void ds_set_property(GObject *o, guint id, const GValue *v, GParamSpec *ps) {
    DsTranscript *t = DS_TRANSCRIPT(o);
    switch (id) {
    case PROP_HADJ: set_hadj(t, g_value_get_object(v)); break;
    case PROP_VADJ: set_vadj(t, g_value_get_object(v)); break;
    case PROP_HPOLICY: t->hpolicy = g_value_get_enum(v); break;
    case PROP_VPOLICY: t->vpolicy = g_value_get_enum(v); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
    }
}

static void ds_get_property(GObject *o, guint id, GValue *v, GParamSpec *ps) {
    DsTranscript *t = DS_TRANSCRIPT(o);
    switch (id) {
    case PROP_HADJ: g_value_set_object(v, t->hadj); break;
    case PROP_VADJ: g_value_set_object(v, t->vadj); break;
    case PROP_HPOLICY: g_value_set_enum(v, t->hpolicy); break;
    case PROP_VPOLICY: g_value_set_enum(v, t->vpolicy); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
    }
}

static void ds_size_allocate(GtkWidget *w, GtkAllocation *a) {
    DsTranscript *t = DS_TRANSCRIPT(w);
    int old_w = gtk_widget_get_allocated_width(w);
    gboolean at_bottom = ds_transcript_at_bottom(t);
    GTK_WIDGET_CLASS(ds_transcript_parent_class)->size_allocate(w, a);
    if (old_w != a->width) {
        layout_items(t, FALSE);
        relayout_positions(t);
        t->refs_dirty = TRUE;
    }
    update_adjustment(t);
    if (at_bottom) ds_transcript_scroll_to_bottom(t);
}

static void ds_realize(GtkWidget *w) {
    GTK_WIDGET_CLASS(ds_transcript_parent_class)->realize(w);
    DsTranscript *t = DS_TRANSCRIPT(w);
    t->cursor_name = "";
    set_cursor(t, NULL);
}

static void ds_finalize(GObject *o) {
    DsTranscript *t = DS_TRANSCRIPT(o);
    if (t->anim_timer) g_source_remove(t->anim_timer);
    if (t->copied_timer) g_source_remove(t->copied_timer);
    if (t->autoscroll_timer) g_source_remove(t->autoscroll_timer);
    g_ptr_array_unref(t->items);
    g_hash_table_unref(t->by_key);
    g_array_unref(t->refs);
    g_hash_table_unref(t->ref_index);
    g_hash_table_unref(t->env.selection);
    g_free(t->env.hover_key);
    g_free(t->env.hover_item);
    g_free(t->env.copied_key);
    g_free(t->env.pressed_key);
    g_free(t->hover_key_item);
    hit_result_clear(&t->down_hit);
    if (t->vadj) { g_signal_handlers_disconnect_by_data(t->vadj, t); g_object_unref(t->vadj); }
    if (t->hadj) g_object_unref(t->hadj);
    G_OBJECT_CLASS(ds_transcript_parent_class)->finalize(o);
}

static void ds_transcript_class_init(DsTranscriptClass *k) {
    GObjectClass *oc = G_OBJECT_CLASS(k);
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(k);
    oc->set_property = ds_set_property;
    oc->get_property = ds_get_property;
    oc->finalize = ds_finalize;
    wc->draw = ds_draw;
    wc->button_press_event = ds_press;
    wc->button_release_event = ds_release;
    wc->motion_notify_event = ds_motion;
    wc->leave_notify_event = ds_leave;
    wc->key_press_event = ds_key;
    wc->size_allocate = ds_size_allocate;
    wc->realize = ds_realize;
    g_object_class_override_property(oc, PROP_HADJ, "hadjustment");
    g_object_class_override_property(oc, PROP_VADJ, "vadjustment");
    g_object_class_override_property(oc, PROP_HPOLICY, "hscroll-policy");
    g_object_class_override_property(oc, PROP_VPOLICY, "vscroll-policy");
    gtk_widget_class_set_css_name(wc, "transcript");
}

static void ds_transcript_init(DsTranscript *t) {
    t->items = g_ptr_array_new_with_free_func(item_free);
    t->by_key = g_hash_table_new(g_str_hash, g_str_equal);
    t->refs = g_array_new(FALSE, FALSE, sizeof(TextRef));
    t->ref_index = g_hash_table_new(NULL, NULL);
    t->env.selection = g_hash_table_new(NULL, NULL);
    t->env.scale = 1;
    t->max_col = 780;
    t->side_pad = 32;
    t->top_inset = 16;
    t->bottom_inset = 160;
    t->refs_dirty = TRUE;
    gtk_widget_set_can_focus(GTK_WIDGET(t), TRUE);
    gtk_widget_add_events(GTK_WIDGET(t), GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK |
                                             GDK_KEY_PRESS_MASK | GDK_SMOOTH_SCROLL_MASK);
}

GtkWidget *ds_transcript_new(void) { return g_object_new(DS_TYPE_TRANSCRIPT, NULL); }

void ds_transcript_set_hit_handler(DsTranscript *t, TranscriptHitFn fn, gpointer data) {
    t->hit_fn = fn;
    t->hit_data = data;
}

void ds_transcript_set_type_handler(DsTranscript *t, TranscriptTypeFn fn, gpointer data) {
    t->type_fn = fn;
    t->type_data = data;
}

void ds_transcript_set_scroll_handler(DsTranscript *t, void (*fn)(gpointer), gpointer data) {
    t->scroll_fn = fn;
    t->scroll_data = data;
}

void ds_transcript_set_insets(DsTranscript *t, double top, double bottom, double scrim) {
    gboolean scrim_changed = fabs(scrim - t->scrim) > 0.5;
    t->scrim = scrim;
    if (fabs(top - t->top_inset) > 0.5 || fabs(bottom - t->bottom_inset) > 0.5) {
        t->top_inset = top;
        t->bottom_inset = bottom;
        relayout_positions(t);
        t->refs_dirty = TRUE;
        gtk_widget_queue_draw(GTK_WIDGET(t));
    } else if (scrim_changed) gtk_widget_queue_draw(GTK_WIDGET(t));
}

gboolean ds_transcript_at_bottom(DsTranscript *t) {
    if (!t->vadj) return TRUE;
    double v = gtk_adjustment_get_value(t->vadj), page = gtk_adjustment_get_page_size(t->vadj), upper = gtk_adjustment_get_upper(t->vadj);
    return v + page >= upper - 60;
}

void ds_transcript_scroll_to_bottom(DsTranscript *t) {
    if (!t->vadj) return;
    double h = gtk_widget_get_allocated_height(GTK_WIDGET(t));
    double target = MAX(0, MAX(t->content_h, h) - h);
    if (fabs(gtk_adjustment_get_value(t->vadj) - target) > 0.5) gtk_adjustment_set_value(t->vadj, target);
}

void ds_transcript_scroll_to(DsTranscript *t, double y) {
    if (t->vadj) gtk_adjustment_set_value(t->vadj, y);
}

double ds_transcript_content_height(DsTranscript *t) { return t->content_h; }

#ifdef DSN_TEST_HOOKS
gboolean ds_transcript_find_hit(DsTranscript *t, const char *needle, double *wx, double *wy) {
    double off = offset(t), h = gtk_widget_get_allocated_height(GTK_WIDGET(t));
    double x0 = col_x(t), cw = col_width(t);
    for (int i = first_visible(t, off); i < (int)t->items->len; i++) {
        Item *it = g_ptr_array_index(t->items, i);
        if (it->y > off + h) break;
        for (double y = MAX(it->y, off); y < MIN(it->y + it->h, off + h); y += 3) {
            for (double x = x0; x < x0 + cw; x += 4) {
                HitResult r = { 0 };
                if (el_hit(it->el, x - x0, y - it->y, &r) && r.key && strstr(r.key, needle)) {
                    hit_result_clear(&r);
                    *wx = x;
                    *wy = y - off;
                    return TRUE;
                }
                hit_result_clear(&r);
            }
        }
    }
    return FALSE;
}
#endif

#ifdef DSN_TEST_HOOKS
/* Renders the whole document (not just the visible part) into PNG pages of at most 1000px;
 * used for automated review. */
gboolean ds_transcript_render_document(DsTranscript *t, const char *path) {
    int W = gtk_widget_get_allocated_width(GTK_WIDGET(t));
    int H = (int)MIN(t->content_h, 60000);
    if (W <= 0 || H <= 0) return FALSE;
    double x = col_x(t);
    for (int page = 0, top = 0; top < H; page++, top += 1000) {
        int ph = MIN(1000, H - top);
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, ph);
        cairo_t *cr = cairo_create(s);
        set_color(cr, C_BG);
        cairo_paint(cr);
        cairo_translate(cr, 0, -top);
        DrawEnv env = t->env;
        env.clip_top = top;
        env.clip_bottom = top + ph;
        for (guint i = 0; i < t->items->len; i++) {
            Item *it = g_ptr_array_index(t->items, i);
            if (it->y > top + ph) break;
            el_draw(it->el, cr, x, it->y, &env);
        }
        cairo_destroy(cr);
        char *fn = g_strdup_printf("%s-%d.png", path, page);
        cairo_surface_write_to_png(s, fn);
        g_free(fn);
        cairo_surface_destroy(s);
    }
    return TRUE;
}
#endif
