#include "chat.h"
#include "transcript.h"
#include "composer.h"
#include "agent.h"
#include "store.h"
#include "theme.h"
#include <string.h>
#include <math.h>

struct Chat {
    ChatCallbacks cb;
    GtkWidget *overlay;
    GtkWidget *scroll;
    DsTranscript *transcript;
    Composer *composer;
    GtkWidget *status;
    GtkWidget *plan;
    GtkWidget *empty;
    GtkWidget *empty_ws_label;
    GtkWidget *down_button;
    Session *session;
    char *pending_ws;
    TranscriptState *state;
    guint rebuild_source;
    guint tick_source;
    gboolean stick;
    gboolean programmatic;
    gboolean plan_expanded;
    double comp_top; /* y of the composer top in overlay coordinates */
    gboolean dead;
};

static void rebuild(Chat *c);

GtkWidget *chat_widget(Chat *c) { return c->overlay; }
GtkWidget *chat_transcript(Chat *c) { return GTK_WIDGET(c->transcript); }
Session *chat_session(Chat *c) { return c->session; }
const char *chat_pending_workspace(Chat *c) { return c->pending_ws; }
const char *chat_workspace(Chat *c) { return c->session ? c->session->cwd : c->pending_ws; }

static gboolean is_empty(Chat *c) { return !c->session || c->session->messages->len == 0; }

/* ---------------- layout ---------------- */

static double column_width(Chat *c, double W) { return MIN(780 + 24, W - 32); }

static int plan_height(Chat *c) {
    if (!c->session) return 36;
    int n = MIN((int)c->session->todos->len, 12);
    return c->plan_expanded ? 36 + n * 24 + 6 : 36;
}

static gboolean child_position(GtkOverlay *o, GtkWidget *w, GdkRectangle *a, Chat *c) {
    int W = gtk_widget_get_allocated_width(GTK_WIDGET(o)), H = gtk_widget_get_allocated_height(GTK_WIDGET(o));
    double colw = column_width(c, W);
    double colx = (W - colw) / 2;
    int cmin, cnat;
    gtk_widget_get_preferred_height_for_width(composer_widget(c->composer), colw, &cmin, &cnat);
    double comp_h = cnat;
    gboolean empty = is_empty(c);
    double comp_y = empty ? H / 2.0 - comp_h + 40 : H - 30 - comp_h;
    if (w == composer_widget(c->composer)) {
        *a = (GdkRectangle){ (int)colx, (int)comp_y, (int)colw, (int)comp_h };
        return TRUE;
    }
    if (w == c->status) {
        *a = (GdkRectangle){ (int)colx, H - 26, (int)colw, 20 };
        return TRUE;
    }
    double top = comp_y;
    if (gtk_widget_get_visible(c->plan)) {
        int ph = plan_height(c);
        top -= ph + 8;
        if (w == c->plan) {
            *a = (GdkRectangle){ (int)colx + 12, (int)top, (int)colw - 24, ph };
            return TRUE;
        }
    }
    if (w == c->down_button) {
        int mw, mh;
        gtk_widget_get_preferred_width(w, &mw, NULL);
        gtk_widget_get_preferred_height(w, &mh, NULL);
        *a = (GdkRectangle){ W / 2 - mw / 2, (int)top - mh - 8, mw, mh };
        return TRUE;
    }
    if (w == c->empty) {
        int mh;
        gtk_widget_get_preferred_height_for_width(w, colw, NULL, &mh);
        *a = (GdkRectangle){ (int)colx, (int)(comp_y - mh - 18), (int)colw, mh };
        return TRUE;
    }
    if (fabs(c->comp_top - top) > 0.5) {
        c->comp_top = top;
    }
    return FALSE;
}

static void update_insets(Chat *c) {
    GtkWidget *o = c->overlay;
    int W = gtk_widget_get_allocated_width(o), H = gtk_widget_get_allocated_height(o);
    if (W <= 1) return;
    double colw = column_width(c, W);
    int cmin, cnat;
    gtk_widget_get_preferred_height_for_width(composer_widget(c->composer), colw, &cmin, &cnat);
    double comp_y = H - 30 - cnat;
    double top = comp_y;
    if (gtk_widget_get_visible(c->plan)) top -= plan_height(c) + 8;
    ds_transcript_set_insets(c->transcript, 16, (H - top) + 40, (H - comp_y) + cnat * 0.55);
}

static void overlay_allocated(GtkWidget *w, GdkRectangle *a, Chat *c) { update_insets(c); }

/* ---------------- plan bar ---------------- */

static gboolean plan_draw(GtkWidget *w, cairo_t *cr, Chat *c) {
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    fill_rounded(cr, 0.5, 0.5, W - 1, H - 1, 14, th(C_CARD_BG));
    stroke_rounded(cr, 0, 0, W, H, 14, th(C_CARD_BORDER), 1);
    if (!c->session) return TRUE;
    GPtrArray *todos = c->session->todos;
    int done = 0;
    TodoItem *cur = NULL;
    for (guint i = 0; i < todos->len; i++) {
        TodoItem *t = g_ptr_array_index(todos, i);
        if (!strcmp(t->status, "completed")) done++;
        if (!cur && !strcmp(t->status, "in_progress")) cur = t;
    }
    for (guint i = 0; i < todos->len && !cur; i++) {
        TodoItem *t = g_ptr_array_index(todos, i);
        if (!strcmp(t->status, "pending")) cur = t;
    }
    draw_icon(cr, "view-list-symbolic", 12, 0, 16, 36, 14, th(C_TEXT2));
    char *head = g_strdup_printf("Plan  %d/%u", done, todos->len);
    double hw = draw_label(cr, head, 34, 0, 120, 36, F_BODY, FS_SEMIBOLD, th(C_TEXT), PANGO_ALIGN_LEFT);
    g_free(head);
    if (cur && !c->plan_expanded) draw_label(cr, cur->content, 44 + hw, 0, W - hw - 90, 36, F_SMALL, 0, th(C_TEXT2), PANGO_ALIGN_LEFT);
    draw_chevron(cr, W - 20, 18, 8, c->plan_expanded ? 0 : 1, th(C_TEXT3));
    if (!c->plan_expanded) return TRUE;
    double y = 36;
    for (guint i = 0; i < todos->len && i < 12; i++) {
        TodoItem *t = g_ptr_array_index(todos, i);
        double bx = 14, by = y + 5;
        if (!strcmp(t->status, "completed")) {
            fill_rounded(cr, bx, by, 14, 14, 4, th(C_ACCENT));
            draw_check(cr, bx, by, 14, 14, th(C_ON_ACCENT), 1.6);
        } else if (!strcmp(t->status, "in_progress")) {
            stroke_rounded(cr, bx, by, 14, 14, 4, th(C_ACCENT), 1.4);
            fill_rounded(cr, bx + 4, by + 4, 6, 6, 2, th(C_ACCENT));
        } else stroke_rounded(cr, bx, by, 14, 14, 4, th(C_TEXT3), 1.2);
        gboolean completed = !strcmp(t->status, "completed");
        draw_label(cr, t->content, 36, y + 2, W - 50, 20, !strcmp(t->status, "in_progress") ? F_BODY : F_SMALL,
                   (!strcmp(t->status, "in_progress") ? FS_SEMIBOLD : 0) | (completed ? FS_STRIKE : 0), th(completed ? C_TEXT3 : C_TEXT), PANGO_ALIGN_LEFT);
        y += 24;
    }
    return TRUE;
}

static gboolean plan_click(GtkWidget *w, GdkEventButton *ev, Chat *c) {
    if (ev->button != 1) return FALSE;
    c->plan_expanded = !c->plan_expanded;
    gtk_widget_queue_resize(c->overlay);
    gtk_widget_queue_draw(w);
    update_insets(c);
    return TRUE;
}

/* ---------------- empty state ---------------- */

static gboolean logo_draw(GtkWidget *w, cairo_t *cr, gpointer p) {
    double s = MIN(gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
    draw_logo(cr, 0, 0, s, th(C_ACCENT));
    return TRUE;
}

static void ws_clicked(GtkButton *b, Chat *c) {
    if (c->cb.request_workspace) c->cb.request_workspace(c->cb.data);
}

static GtkWidget *build_empty(Chat *c) {
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    GtkWidget *logo = gtk_drawing_area_new();
    gtk_widget_set_size_request(logo, 44, 44);
    g_signal_connect(logo, "draw", G_CALLBACK(logo_draw), NULL);
    gtk_box_pack_start(GTK_BOX(row), logo, FALSE, FALSE, 0);
    GtkWidget *title = gtk_label_new("Into the Unknown");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "empty-title");
    gtk_box_pack_start(GTK_BOX(row), title, FALSE, FALSE, 0);
    GtkWidget *badge = gtk_label_new("Native");
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(badge), "badge");
    gtk_box_pack_start(GTK_BOX(row), badge, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), row, FALSE, FALSE, 0);
    GtkWidget *b = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_halign(b, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "workspace-button");
    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(bb), gtk_image_new_from_icon_name("folder-symbolic", GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    c->empty_ws_label = gtk_label_new("Choose workspace");
    gtk_box_pack_start(GTK_BOX(bb), c->empty_ws_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bb), gtk_image_new_from_icon_name("pan-down-symbolic", GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(b), bb);
    g_signal_connect(b, "clicked", G_CALLBACK(ws_clicked), c);
    gtk_box_pack_start(GTK_BOX(v), b, FALSE, FALSE, 0);
    return v;
}

static void set_label(GtkWidget *l, const char *text) {
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(l)), text)) gtk_label_set_text(GTK_LABEL(l), text);
}

static void update_empty(Chat *c) {
    gboolean empty = is_empty(c);
    const char *ws = chat_workspace(c);
    set_label(c->empty_ws_label, ws ? path_base(ws) : "Choose workspace");
    if (gtk_widget_get_visible(c->empty) == empty && gtk_widget_get_visible(c->scroll) == !empty) return;
    gtk_widget_set_visible(c->empty, empty);
    gtk_widget_set_visible(c->scroll, !empty);
    gtk_widget_set_visible(c->status, !empty);
    gtk_widget_queue_resize(c->overlay);
}

/* ---------------- status ---------------- */

static void update_status(Chat *c) {
    Session *s = c->session;
    if (!s || !s->messages->len) { set_label(c->status, ""); return; }
    GString *t = g_string_new("");
    g_string_append_printf(t, "%u turn%s %d steps", s->turns->len, s->turns->len == 1 ? "" : "s", session_step_count(s));
    double rate = session_output_rate(s);
    if (rate >= 0) g_string_append_printf(t, "  ·  %d tok/s", (int)rate);
    Usage u = session_total_usage(s);
    char *tk = fmt_tokens(usage_total(&u));
    g_string_append_printf(t, "  ·  %s tok", tk);
    g_free(tk);
    double ch = session_cache_hit_rate(s);
    if (ch >= 0) g_string_append_printf(t, "  ·  Cache hit %d%%", (int)(ch * 100));
    double ctx = (double)session_last_prompt_tokens(s) / MAX(1, settings()->context_window);
    g_string_append_printf(t, "  ·  Context %d%%", (int)(ctx * 100 + 0.5));
    set_label(c->status, t->str);
    g_string_free(t, TRUE);
}

/* ---------------- rebuild ---------------- */

static void update_down_button(Chat *c) {
    gboolean show = !is_empty(c) && !ds_transcript_at_bottom(c->transcript);
    gtk_widget_set_visible(c->down_button, show);
}

static void rebuild(Chat *c) {
    if (c->rebuild_source) { g_source_remove(c->rebuild_source); c->rebuild_source = 0; }
    if (c->dead) return;
    Session *s = c->session;
    if (!s) {
        ds_transcript_clear(c->transcript);
        update_status(c);
        gtk_widget_hide(c->plan);
        composer_set_running(c->composer, FALSE);
        update_empty(c);
        return;
    }
    gint64 t0 = g_get_monotonic_time();
    GPtrArray *specs = transcript_build(s, c->state, ds_transcript_have, c->transcript);
    gint64 t1 = g_get_monotonic_time();
    ds_transcript_apply(c->transcript, specs);
    perf_note("build", t1 - t0);
    perf_note("apply", g_get_monotonic_time() - t1);
    composer_set_running(c->composer, s->running);
    composer_set_placeholder(c->composer, s->pending_questions->len ? "Type your answer…"
                                          : s->running ? "Send a message to steer the agent…"
                                                       : "Message or run a task, / commands, @ files");
    gboolean all_done = TRUE;
    for (guint i = 0; i < s->todos->len; i++)
        if (strcmp(((TodoItem *)g_ptr_array_index(s->todos, i))->status, "completed")) all_done = FALSE;
    gboolean plan_visible = s->todos->len > 0 && !(all_done && !s->running);
    if (plan_visible != gtk_widget_get_visible(c->plan)) {
        gtk_widget_set_visible(c->plan, plan_visible);
        update_insets(c);
    }
    if (plan_visible) gtk_widget_queue_draw(c->plan);
    update_status(c);
    update_empty(c);
    if (c->stick) {
        c->programmatic = TRUE;
        ds_transcript_scroll_to_bottom(c->transcript);
        c->programmatic = FALSE;
    }
    update_down_button(c);
}

static gboolean rebuild_idle(gpointer p) {
    Chat *c = p;
    c->rebuild_source = 0;
    rebuild(c);
    return G_SOURCE_REMOVE;
}

static void schedule_rebuild(Chat *c) {
    if (c->dead) return;
    if (!c->rebuild_source) c->rebuild_source = g_idle_add_full(G_PRIORITY_HIGH_IDLE + 15, rebuild_idle, c, NULL);
}

static gboolean tick(gpointer p) {
    Chat *c = p;
    if (c->dead || !c->session || !c->session->running) { c->tick_source = 0; return G_SOURCE_REMOVE; }
    TurnRecord *t = session_last_turn(c->session);
    if (t) {
        char *k = g_strdup_printf("h%d", t->index);
        ds_transcript_invalidate_item(c->transcript, k);
        g_free(k);
    }
    return G_SOURCE_CONTINUE;
}

static void update_ticker(Chat *c) {
    gboolean running = c->session && c->session->running;
    if (running && !c->tick_source) c->tick_source = g_timeout_add(1000, tick, c);
}

static void on_session_change(Session *s, ChangeKind kind, const char *tool_id, gpointer data) {
    Chat *c = data;
    if (s != c->session || c->dead) return;
    schedule_rebuild(c);
    if (kind == CHANGE_META || kind == CHANGE_RESET) {
        update_ticker(c);
        if (c->cb.meta_changed) c->cb.meta_changed(c->cb.data);
    }
}

void chat_show(Chat *c, Session *s) {
    if (c->dead || (s && s == c->session)) return;
    if (c->session) session_unobserve(c->session, c);
    c->session = s;
    tstate_reset(c->state);
    ds_transcript_clear_selection(c->transcript);
    ds_transcript_clear(c->transcript);
    if (s) {
        session_load(s);
        session_observe(s, on_session_change, c);
        composer_set_permission(c->composer, s->permission);
        composer_set_model(c->composer, s->model, s->effort);
    } else {
        composer_set_permission(c->composer, settings()->permission);
        composer_set_model(c->composer, settings()->model, settings()->effort);
    }
    c->stick = TRUE;
    rebuild(c);
    c->programmatic = TRUE;
    ds_transcript_scroll_to_bottom(c->transcript);
    c->programmatic = FALSE;
    update_ticker(c);
    composer_focus(c->composer);
}

void chat_set_pending_workspace(Chat *c, const char *path) {
    char *p = g_strdup(path);
    g_free(c->pending_ws);
    c->pending_ws = p;
    update_empty(c);
}

/* ---------------- scrolling ---------------- */

static void on_scrolled(gpointer p) {
    Chat *c = p;
    if (!c->programmatic) c->stick = ds_transcript_at_bottom(c->transcript);
    update_down_button(c);
}

static void down_clicked(GtkButton *b, Chat *c) {
    c->stick = TRUE;
    ds_transcript_scroll_to_bottom(c->transcript);
    update_down_button(c);
}

/* ---------------- links / hits ---------------- */

static void open_uri(Chat *c, const char *uri) {
    GtkWidget *top = gtk_widget_get_toplevel(c->overlay);
    GError *err = NULL;
    if (g_str_has_prefix(uri, "file://")) {
        char *path = g_strdup(uri + 7);
        char *hash = strchr(path, '#');
        if (hash) *hash = 0;
        char *un = g_uri_unescape_string(path, NULL);
        if (un) { g_free(path); path = un; }
        if (!path_exists(path)) {
            gtk_widget_error_bell(c->overlay);
            g_free(path);
            return;
        }
        char *u = g_filename_to_uri(path, NULL, NULL);
        ui_show_uri(top, u);
        g_free(u);
        g_free(path);
    } else {
        ui_show_uri(top, uri);
    }
    if (err) {
        dlog("open %s failed: %s", uri, err->message);
        g_error_free(err);
    }
}

static QuestionRequest *find_question(Session *s, const char *id) {
    for (guint i = 0; i < s->pending_questions->len; i++) {
        QuestionRequest *q = g_ptr_array_index(s->pending_questions, i);
        if (!strcmp(q->id, id)) return q;
    }
    return NULL;
}

static void on_hit(const Hit *h, gpointer data) {
    Chat *c = data;
    Session *s = c->session;
    if (!s) return;
    switch (h->kind) {
    case HIT_TOGGLE: {
        const char *key = h->a;
        if (g_hash_table_contains(c->state->effective_expanded, key)) {
            g_hash_table_remove(c->state->expanded, key);
            g_hash_table_add(c->state->collapsed, g_strdup(key));
        } else {
            g_hash_table_add(c->state->expanded, g_strdup(key));
            g_hash_table_remove(c->state->collapsed, key);
        }
        gboolean was = c->stick;
        c->stick = FALSE;
        rebuild(c);
        c->stick = was && ds_transcript_at_bottom(c->transcript);
        break;
    }
    case HIT_LINK: open_uri(c, h->a); break;
    case HIT_OPEN_FILE: {
        char *u = g_strconcat("file://", h->a, NULL);
        open_uri(c, u);
        g_free(u);
        break;
    }
    case HIT_APPROVE:
        if (s->agent) agent_resolve_approval(s->agent, h->a, h->n);
        break;
    case HIT_CHOOSE: {
        QuestionRequest *q = find_question(s, h->a);
        if (!q) break;
        Question *qq = NULL;
        for (guint i = 0; i < q->questions->len; i++)
            if (!strcmp(((Question *)g_ptr_array_index(q->questions, i))->id, h->b)) qq = g_ptr_array_index(q->questions, i);
        if (!qq) break;
        tstate_toggle_option(c->state, h->a, h->b, h->c, qq->multi);
        if (q->questions->len == 1 && !qq->multi) {
            JsonObject *ans = jo_new();
            JsonArray *arr = json_array_new();
            json_array_add_string_element(arr, h->c);
            jo_arr(ans, qq->id, arr);
            if (s->agent) agent_resolve_question(s->agent, q->id, ans);
            else json_object_unref(ans);
        } else rebuild(c);
        break;
    }
    case HIT_SUBMIT: {
        QuestionRequest *q = find_question(s, h->a);
        if (!q) break;
        JsonObject *ans = jo_new();
        for (guint i = 0; i < q->questions->len; i++) {
            Question *qq = g_ptr_array_index(q->questions, i);
            GPtrArray *sel = tstate_options(c->state, q->id, qq->id);
            JsonArray *arr = json_array_new();
            for (guint k = 0; k < sel->len; k++) json_array_add_string_element(arr, g_ptr_array_index(sel, k));
            jo_arr(ans, qq->id, arr);
            g_ptr_array_unref(sel);
        }
        if (s->agent) agent_resolve_question(s->agent, q->id, ans);
        else json_object_unref(ans);
        break;
    }
    case HIT_DISMISS:
        if (s->agent) agent_resolve_question(s->agent, h->a, NULL);
        break;
    case HIT_RETRY: chat_retry(c); break;
    case HIT_CANCEL_QUEUED:
        if (h->n >= 0 && h->n < (int)s->queued_inputs->len) {
            g_ptr_array_remove_index(s->queued_inputs, h->n);
            session_notify(s, CHANGE_META, NULL);
        }
        break;
    default: break;
    }
}

static void on_type(GdkEventKey *ev, gpointer data) {
    Chat *c = data;
    composer_forward_key(c->composer, ev);
}

/* ---------------- actions ---------------- */

void chat_retry(Chat *c) {
    Session *s = c->session;
    if (!s || s->running) return;
    for (int i = s->messages->len - 1; i >= 0; i--) {
        Message *m = g_ptr_array_index(s->messages, i);
        if (m->role == ROLE_USER && !m->is_tool_results && !m->notice) {
            char *t = message_text(m);
            c->stick = TRUE;
            agent_submit(agent_for(s), t, NULL);
            g_free(t);
            update_ticker(c);
            return;
        }
    }
}

void chat_stop(Chat *c) {
    if (c->session && c->session->agent) agent_cancel(c->session->agent);
}

void chat_focus(Chat *c) { composer_focus(c->composer); }

char *chat_last_response(Chat *c) {
    if (!c->session) return NULL;
    const Message *m = session_last_assistant_with_text(c->session);
    return m ? message_text(m) : NULL;
}

void chat_copy_last(Chat *c) {
    char *t = chat_last_response(c);
    if (t) gtk_clipboard_set_text(gtk_widget_get_clipboard(c->overlay, GDK_SELECTION_CLIPBOARD), t, -1);
    g_free(t);
}

void chat_export(Chat *c) {
    Session *s = c->session;
    if (!s || !s->messages->len) return;
    GtkWidget *top = gtk_widget_get_toplevel(c->overlay);
    GtkFileChooserNative *fc = gtk_file_chooser_native_new("Export as Markdown", GTK_WINDOW(top), GTK_FILE_CHOOSER_ACTION_SAVE, "_Export", "_Cancel");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(fc), TRUE);
    char *name = str_replace(s->title, "/", "-");
    char *fn = g_strconcat(name, ".md", NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), fn);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), s->cwd);
    g_free(fn);
    g_free(name);
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        char *md = session_export_markdown(s);
        g_file_set_contents(path, md, -1, NULL);
        g_free(md);
        g_free(path);
    }
    g_object_unref(fc);
}

void chat_theme_changed(Chat *c) {
    ds_transcript_relayout_all(c->transcript);
    gtk_widget_queue_draw(c->overlay);
}

void chat_expand_all(Chat *c) {
    Session *s = c->session;
    if (!s) return;
    GPtrArray *keys = ds_transcript_keys(c->transcript);
    for (guint i = 0; i < keys->len; i++) {
        const char *k = g_ptr_array_index(keys, i);
        if (g_str_has_prefix(k, "g.")) g_hash_table_add(c->state->expanded, g_strdup(k));
    }
    g_ptr_array_unref(keys);
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init(&it, s->tool_runs);
    while (g_hash_table_iter_next(&it, &k, NULL)) g_hash_table_add(c->state->expanded, g_strdup(k));
    c->stick = FALSE;
    rebuild(c);
}

void chat_compose(Chat *c, const char *text) {
    composer_set_text(c->composer, text);
    composer_focus(c->composer);
}

/* ---------------- composer callbacks ---------------- */

static void comp_send(const char *text, GPtrArray *images, gpointer data) {
    Chat *c = data;
    Session *s = c->session;
    gboolean created = FALSE;
    if (!s) {
        if (!c->pending_ws) {
            g_ptr_array_unref(images);
            if (c->cb.request_workspace) c->cb.request_workspace(c->cb.data);
            return;
        }
        s = store_new_session(c->pending_ws);
        s->permission = composer_permission(c->composer);
        g_free(s->model);
        s->model = g_strdup(composer_model(c->composer));
        s->effort = composer_effort(c->composer);
        session_save_config(s);
        chat_show(c, s);
        created = TRUE;
    }
    if (s->pending_questions->len) {
        QuestionRequest *q = g_ptr_array_index(s->pending_questions, 0);
        JsonObject *ans = jo_new();
        for (guint i = 0; i < q->questions->len; i++) {
            Question *qq = g_ptr_array_index(q->questions, i);
            JsonArray *arr = json_array_new();
            if (i == 0) json_array_add_string_element(arr, text);
            else {
                GPtrArray *sel = tstate_options(c->state, q->id, qq->id);
                for (guint k = 0; k < sel->len; k++) json_array_add_string_element(arr, g_ptr_array_index(sel, k));
                g_ptr_array_unref(sel);
            }
            jo_arr(ans, qq->id, arr);
        }
        g_ptr_array_unref(images);
        if (s->agent) agent_resolve_question(s->agent, q->id, ans);
        else json_object_unref(ans);
        return;
    }
    c->stick = TRUE;
    agent_submit(agent_for(s), text, images->len ? images : (g_ptr_array_unref(images), NULL));
    update_ticker(c);
    if (created && c->cb.started_session) c->cb.started_session(s, c->cb.data);
}

void chat_send_text(Chat *c, const char *text) { comp_send(text, g_ptr_array_new_with_free_func(image_data_free), c); }

static void comp_stop(gpointer data) { chat_stop(data); }

static void comp_perm(PermissionMode m, gpointer data) {
    Chat *c = data;
    if (c->session) { c->session->permission = m; session_save_config(c->session); }
    settings()->permission = m;
    settings_save();
}

static void comp_model(const char *model, Effort e, gpointer data) {
    Chat *c = data;
    if (c->session) {
        g_free(c->session->model);
        c->session->model = g_strdup(model);
        c->session->effort = e;
        session_save_config(c->session);
    }
    settings_set_model(model);
    settings()->effort = e;
    settings_save();
}

static void comp_slash(const char *cmd, gpointer data) {
    Chat *c = data;
    if (!strcmp(cmd, "/new")) { if (c->cb.request_new) c->cb.request_new(c->cb.data); }
    else if (!strcmp(cmd, "/stop")) chat_stop(c);
    else if (!strcmp(cmd, "/retry")) chat_retry(c);
    else if (!strcmp(cmd, "/rename")) { if (c->cb.request_rename) c->cb.request_rename(c->cb.data); }
    else if (!strcmp(cmd, "/copy")) chat_copy_last(c);
    else if (!strcmp(cmd, "/export")) chat_export(c);
    else if (!strcmp(cmd, "/settings")) { if (c->cb.request_settings) c->cb.request_settings(c->cb.data); }
    else if (!strcmp(cmd, "/workspace")) { if (c->cb.request_workspace) c->cb.request_workspace(c->cb.data); }
}

static const char *comp_workspace(gpointer data) { return chat_workspace(data); }

/* ---------------- construction ---------------- */

static void overlay_destroyed(GtkWidget *w, Chat *c) {
    c->dead = TRUE;
    if (c->session) session_unobserve(c->session, c);
    c->session = NULL;
    if (c->rebuild_source) { g_source_remove(c->rebuild_source); c->rebuild_source = 0; }
    if (c->tick_source) { g_source_remove(c->tick_source); c->tick_source = 0; }
}

Chat *chat_new(ChatCallbacks cb) {
    Chat *c = g_new0(Chat, 1);
    c->cb = cb;
    c->state = tstate_new();
    c->stick = TRUE;
    c->overlay = gtk_overlay_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(c->overlay), "chat");
    c->transcript = DS_TRANSCRIPT(ds_transcript_new());
    ds_transcript_set_hit_handler(c->transcript, on_hit, c);
    ds_transcript_set_type_handler(c->transcript, on_type, c);
    ds_transcript_set_scroll_handler(c->transcript, on_scrolled, c);
    c->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(c->scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(c->scroll), TRUE);
    gtk_container_add(GTK_CONTAINER(c->scroll), GTK_WIDGET(c->transcript));
    gtk_container_add(GTK_CONTAINER(c->overlay), c->scroll);

    c->empty = build_empty(c);
    gtk_overlay_add_overlay(GTK_OVERLAY(c->overlay), c->empty);

    c->plan = gtk_drawing_area_new();
    gtk_widget_add_events(c->plan, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(c->plan, "draw", G_CALLBACK(plan_draw), c);
    g_signal_connect(c->plan, "button-press-event", G_CALLBACK(plan_click), c);
    gtk_widget_set_no_show_all(c->plan, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(c->overlay), c->plan);

    ComposerCallbacks ccb = { comp_send, comp_stop, comp_perm, comp_model, comp_slash, comp_workspace, c };
    c->composer = composer_new(ccb);
    gtk_overlay_add_overlay(GTK_OVERLAY(c->overlay), composer_widget(c->composer));

    c->status = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(c->status), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(c->status), "status-line");
    gtk_overlay_add_overlay(GTK_OVERLAY(c->overlay), c->status);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(c->overlay), c->status, TRUE);

    c->down_button = gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_style_context_add_class(gtk_widget_get_style_context(c->down_button), "circular");
    gtk_style_context_add_class(gtk_widget_get_style_context(c->down_button), "scroll-down");
    gtk_widget_set_tooltip_text(c->down_button, "Scroll to bottom");
    g_signal_connect(c->down_button, "clicked", G_CALLBACK(down_clicked), c);
    gtk_widget_set_no_show_all(c->down_button, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(c->overlay), c->down_button);

    g_signal_connect(c->overlay, "get-child-position", G_CALLBACK(child_position), c);
    g_signal_connect(c->overlay, "destroy", G_CALLBACK(overlay_destroyed), c);
    g_signal_connect_after(c->overlay, "size-allocate", G_CALLBACK(overlay_allocated), c);
    gtk_widget_show_all(c->overlay);
    update_empty(c);
    return c;
}

void chat_debug_popovers(Chat *c, GtkWidget **out, int *n) { composer_debug_popovers(c->composer, out, n); }
