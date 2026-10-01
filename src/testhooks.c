/* Automated UI test hooks, compiled only with -DDSN_TEST_HOOKS (make BUILD=debug or TEST_HOOKS=1).
 * Release builds cannot be driven through these environment variables. */
#include "testhooks.h"

#ifdef DSN_TEST_HOOKS
#include "store.h"
#include "agent.h"
#include "theme.h"
#include "transcript.h"
#include <stdio.h>
#include <string.h>

static TestHooksCtx HK;
static int snapshot_n;
static guint snapshot_source;

void testhooks_window_size(int *w, int *h) {
    const char *size = g_getenv("DSN_WINDOW_SIZE");
    if (size) sscanf(size, "%dx%d", w, h);
}

gboolean testhooks_suppress_key_prompt(void) { return g_getenv("DSN_NO_KEY_PROMPT") != NULL; }


static gboolean snapshot_tick(gpointer p) {
    const char *path = g_getenv("DSN_SNAPSHOT");
    if (!path || !HK.window()) return G_SOURCE_REMOVE;
    snapshot_n++;
    GList *tops = gtk_window_list_toplevels();
    int k = 0;
    for (GList *l = tops; l; l = l->next) {
        GtkWidget *w = l->data;
        if (!gtk_widget_get_visible(w) || !gtk_widget_is_toplevel(w) || GTK_IS_WINDOW(w) == FALSE) continue;
        if (gtk_window_get_window_type(GTK_WINDOW(w)) != GTK_WINDOW_TOPLEVEL) continue;
        int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
        if (W < 10 || H < 10) continue;
        int scale = gtk_widget_get_scale_factor(w);
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W * scale, H * scale);
        cairo_surface_set_device_scale(s, scale, scale);
        cairo_t *cr = cairo_create(s);
        gtk_widget_draw(w, cr);
        cairo_destroy(cr);
        char *fn = w == HK.window() ? g_strdup_printf("%s-%d.png", path, snapshot_n) : g_strdup_printf("%s-%d-w%d.png", path, snapshot_n, k++);
        cairo_surface_write_to_png(s, fn);
        g_free(fn);
        cairo_surface_destroy(s);
    }
    g_list_free(tops);
    GtkWidget *pops[4];
    int np = 0;
    chat_debug_popovers(HK.chat, pops, &np);
    for (int i = 0; i < np; i++) {
        int W = gtk_widget_get_allocated_width(pops[i]), H = gtk_widget_get_allocated_height(pops[i]);
        if (W < 2 || H < 2) continue;
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        cairo_t *cr = cairo_create(s);
        gtk_widget_draw(pops[i], cr);
        cairo_destroy(cr);
        char *fn = g_strdup_printf("%s-%d-pop%d.png", path, snapshot_n, i);
        cairo_surface_write_to_png(s, fn);
        g_free(fn);
        cairo_surface_destroy(s);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean autosend_cb(gpointer p) {
    const char *t = g_getenv("DSN_AUTOSEND");
    if (t) chat_send_text(HK.chat, t);
    return G_SOURCE_REMOVE;
}

static gboolean compose_cb(gpointer p) {
    chat_compose(HK.chat, g_getenv("DSN_COMPOSE"));
    return G_SOURCE_REMOVE;
}

static gboolean expand_cb(gpointer p) {
    chat_expand_all(HK.chat);
    return G_SOURCE_REMOVE;
}

static gboolean quit_cb(gpointer p) {
    GPtrArray *running = store_running_sessions();
    for (guint i = 0; i < running->len; i++) {
        Session *s = g_ptr_array_index(running, i);
        if (s->agent) agent_cancel(s->agent);
    }
    g_ptr_array_unref(running);
    HK.quit();
    return G_SOURCE_REMOVE;
}

static void autoanswer_event(Session *s, gboolean attention, gpointer d);

typedef struct { char *sid; } AnswerCtx;

static gboolean autoanswer_cb(gpointer p) {
    AnswerCtx *c = p;
    Session *s = store_live(c->sid);
    if (s && s->agent) {
        while (s->pending_approvals->len) {
            ApprovalRequest *a = g_ptr_array_index(s->pending_approvals, 0);
            agent_resolve_approval(s->agent, a->id, DECIDE_ONCE);
        }
        while (s->pending_questions->len) {
            QuestionRequest *q = g_ptr_array_index(s->pending_questions, 0);
            JsonObject *ans = jo_new();
            for (guint i = 0; i < q->questions->len; i++) {
                Question *qq = g_ptr_array_index(q->questions, i);
                JsonArray *arr = json_array_new();
                const char *l = qq->labels->len > 1 ? g_ptr_array_index(qq->labels, 1) : qq->labels->len ? g_ptr_array_index(qq->labels, 0) : "yes";
                json_array_add_string_element(arr, l);
                jo_arr(ans, qq->id, arr);
            }
            agent_resolve_question(s->agent, q->id, ans);
        }
    }
    g_free(c->sid);
    g_free(c);
    return G_SOURCE_REMOVE;
}

static void autoanswer_event(Session *s, gboolean attention, gpointer d) {
    HK.agent_event(s, attention, d);
    if (!attention) return;
    AnswerCtx *c = g_new0(AnswerCtx, 1);
    c->sid = g_strdup(s->id);
    g_timeout_add((guint)(g_ascii_strtod(g_getenv("DSN_AUTOANSWER"), NULL) * 1000), autoanswer_cb, c);
}


/* DSN_SCRIPT="t:click x,y;t:key Return;t:type text;t:rclick x,y" — synthesized input for automated UI tests. */
static GdkWindow *window_at(GdkWindow *w, int x, int y, int *lx, int *ly) {
    GList *kids = gdk_window_peek_children(w);
    for (GList *l = kids; l; l = l->next) {
        GdkWindow *c = l->data;
        if (!gdk_window_is_visible(c)) continue;
        int cx, cy, cw, ch;
        gdk_window_get_geometry(c, &cx, &cy, &cw, &ch);
        if (x >= cx && y >= cy && x < cx + cw && y < cy + ch) return window_at(c, x - cx, y - cy, lx, ly);
    }
    *lx = x;
    *ly = y;
    return w;
}

static void synth_button(int x, int y, int button, GdkEventType type) {
    GdkWindow *top = gtk_widget_get_window(HK.window());
    int lx, ly;
    GdkWindow *target = window_at(top, x, y, &lx, &ly);
    GdkEvent *ev = gdk_event_new(type);
    ev->button.window = g_object_ref(target);
    ev->button.send_event = TRUE;
    ev->button.time = GDK_CURRENT_TIME;
    ev->button.x = lx;
    ev->button.y = ly;
    ev->button.x_root = x;
    ev->button.y_root = y;
    ev->button.button = button;
    GdkSeat *seat = gdk_display_get_default_seat(gdk_window_get_display(top));
    gdk_event_set_device(ev, gdk_seat_get_pointer(seat));
    gtk_main_do_event(ev);
    gdk_event_free(ev);
}

static void synth_on(GdkWindow *target, int lx, int ly, int button, GdkEventType type) {
    GdkEvent *ev = gdk_event_new(type);
    ev->button.window = g_object_ref(target);
    ev->button.send_event = TRUE;
    ev->button.time = GDK_CURRENT_TIME;
    ev->button.x = lx;
    ev->button.y = ly;
    ev->button.button = button;
    GdkSeat *seat = gdk_display_get_default_seat(gdk_window_get_display(target));
    gdk_event_set_device(ev, gdk_seat_get_pointer(seat));
    gtk_main_do_event(ev);
    gdk_event_free(ev);
}

static void synth_motion(int x, int y) {
    GdkWindow *top = gtk_widget_get_window(HK.window());
    int lx, ly;
    GdkWindow *target = window_at(top, x, y, &lx, &ly);
    GdkEvent *ev = gdk_event_new(GDK_MOTION_NOTIFY);
    ev->motion.window = g_object_ref(target);
    ev->motion.send_event = TRUE;
    ev->motion.time = GDK_CURRENT_TIME;
    ev->motion.x = lx;
    ev->motion.y = ly;
    ev->motion.x_root = x;
    ev->motion.y_root = y;
    GdkSeat *seat = gdk_display_get_default_seat(gdk_window_get_display(top));
    gdk_event_set_device(ev, gdk_seat_get_pointer(seat));
    gtk_main_do_event(ev);
    gdk_event_free(ev);
}

static void synth_key(guint keyval, guint state) {
    GtkWindow *w = GTK_WINDOW(HK.window());
    GList *tops = gtk_window_list_toplevels();
    for (GList *l = tops; l; l = l->next)
        if (GTK_IS_WINDOW(l->data) && gtk_window_is_active(l->data)) w = l->data;
    g_list_free(tops);
    GdkEvent *ev = gdk_event_new(GDK_KEY_PRESS);
    ev->key.window = g_object_ref(gtk_widget_get_window(GTK_WIDGET(w)));
    ev->key.send_event = TRUE;
    ev->key.time = GDK_CURRENT_TIME;
    ev->key.keyval = keyval;
    ev->key.state = state;
    gunichar u = gdk_keyval_to_unicode(keyval);
    char buf[8] = { 0 };
    if (u) g_unichar_to_utf8(u, buf);
    ev->key.string = g_strdup(buf);
    ev->key.length = strlen(buf);
    GdkKeymapKey *keys = NULL;
    gint n = 0;
    if (gdk_keymap_get_entries_for_keyval(gdk_keymap_get_for_display(gdk_display_get_default()), keyval, &keys, &n) && n) ev->key.hardware_keycode = keys[0].keycode;
    g_free(keys);
    GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
    gdk_event_set_device(ev, gdk_seat_get_keyboard(seat));
    gtk_main_do_event(ev);
    ev->type = GDK_KEY_RELEASE;
    gtk_main_do_event(ev);
    gdk_event_free(ev);
}

static gboolean script_step(gpointer p) {
    char *cmd = p;
    int x = 0, y = 0;
    dlog("script: %s", cmd);
    if (sscanf(cmd, "click %d,%d", &x, &y) == 2) {
        synth_motion(x, y);
        synth_button(x, y, 1, GDK_BUTTON_PRESS);
        synth_button(x, y, 1, GDK_BUTTON_RELEASE);
    } else if (g_str_has_prefix(cmd, "clickkey ")) {
        GtkWidget *tw = chat_transcript(HK.chat);
        double tx, ty;
        if (ds_transcript_find_hit(DS_TRANSCRIPT(tw), cmd + 9, &tx, &ty)) {
            GdkWindow *gw = gtk_widget_get_window(tw);
            synth_on(gw, tx, ty, 1, GDK_BUTTON_PRESS);
            synth_on(gw, tx, ty, 1, GDK_BUTTON_RELEASE);
        } else dlog("script: no hit %s", cmd + 9);
    } else if (sscanf(cmd, "dclick %d,%d", &x, &y) == 2) {
        synth_button(x, y, 1, GDK_BUTTON_PRESS);
        synth_button(x, y, 1, GDK_BUTTON_RELEASE);
        synth_button(x, y, 1, GDK_BUTTON_PRESS);
        synth_button(x, y, 1, GDK_2BUTTON_PRESS);
        synth_button(x, y, 1, GDK_BUTTON_RELEASE);
    } else if (sscanf(cmd, "rclick %d,%d", &x, &y) == 2) {
        synth_button(x, y, 3, GDK_BUTTON_PRESS);
        synth_button(x, y, 3, GDK_BUTTON_RELEASE);
    } else if (sscanf(cmd, "move %d,%d", &x, &y) == 2) {
        synth_motion(x, y);
    } else if (g_str_has_prefix(cmd, "key ")) {
        guint state = 0;
        const char *k = cmd + 4;
        if (g_str_has_prefix(k, "ctrl+")) { state |= GDK_CONTROL_MASK; k += 5; }
        if (g_str_has_prefix(k, "shift+")) { state |= GDK_SHIFT_MASK; k += 6; }
        synth_key(gdk_keyval_from_name(k), state);
    } else if (g_str_has_prefix(cmd, "type ")) {
        for (const char *c = cmd + 5; *c; c = g_utf8_next_char(c)) synth_key(gdk_unicode_to_keyval(g_utf8_get_char(c)), 0);
    } else if (!strcmp(cmd, "delete")) {
        Session *cur = chat_session(HK.chat);
        if (cur) {
            char *id = g_strdup(cur->id), *cwd = g_strdup(cur->cwd);
            HK.new_session(cwd);
            store_delete(id);
            g_free(id);
            g_free(cwd);
        }
    } else if (!strcmp(cmd, "stats")) {
        GPtrArray *running = store_running_sessions();
        fprintf(stderr, "[stats] running=%u indexed=%u\n", running->len, store_index()->len);
        g_ptr_array_unref(running);
    } else if (g_str_has_prefix(cmd, "scroll ")) {
        ds_transcript_scroll_to(DS_TRANSCRIPT(chat_transcript(HK.chat)), g_ascii_strtod(cmd + 7, NULL));
    } else if (g_str_has_prefix(cmd, "doc ")) {
        ds_transcript_render_document(DS_TRANSCRIPT(chat_transcript(HK.chat)), cmd + 4);
    } else if (g_str_has_prefix(cmd, "expand")) {
        chat_expand_all(HK.chat);
    } else if (g_str_has_prefix(cmd, "snap ")) {
        const char *base = g_getenv("DSN_SNAPSHOT");
        char *old = g_strdup(base ? base : "/tmp/dsn-snap");
        g_setenv("DSN_SNAPSHOT", cmd + 5, TRUE);
        int n = snapshot_n;
        snapshot_n = -1;
        snapshot_tick(NULL);
        snapshot_n = n;
        g_setenv("DSN_SNAPSHOT", old, TRUE);
        g_free(old);
    }
    g_free(cmd);
    return G_SOURCE_REMOVE;
}

static void run_script(const char *script) {
    char **steps = g_strsplit(script, ";", -1);
    for (int i = 0; steps[i]; i++) {
        char *colon = strchr(steps[i], ':');
        if (!colon) continue;
        *colon = 0;
        double t = g_ascii_strtod(steps[i], NULL);
        g_timeout_add((guint)(t * 1000), script_step, g_strdup(g_strstrip(colon + 1)));
    }
    g_strfreev(steps);
}

void testhooks_run(const TestHooksCtx *ctx) {
    HK = *ctx;
    const char *open = g_getenv("DSN_OPEN");
    if (open) {
        HK.open_session(open);
        if (g_getenv("DSN_EXPAND_ALL")) g_timeout_add(500, expand_cb, NULL);
    }
    const char *perm = g_getenv("DSN_PERMISSION");
    if (perm) settings()->permission = perm_parse(perm);
    const char *ws = g_getenv("DSN_WORKSPACE");
    if (ws) {
        Workspace *w = store_add_workspace(ws);
        store_set_last_workspace(w->id);
        HK.new_session(w->path);
    }
    if (g_getenv("DSN_AUTOSEND")) g_timeout_add(1000, autosend_cb, NULL);
    if (g_getenv("DSN_COMPOSE")) g_timeout_add(1000, compose_cb, NULL);
    if (g_getenv("DSN_AUTOANSWER")) agent_set_event_handler(autoanswer_event, NULL);
    if (g_getenv("DSN_SNAPSHOT")) {
        double iv = g_getenv("DSN_SNAPSHOT_INTERVAL") ? g_ascii_strtod(g_getenv("DSN_SNAPSHOT_INTERVAL"), NULL) : 5;
        snapshot_source = g_timeout_add((guint)(iv * 1000), snapshot_tick, NULL);
    }
    if (g_getenv("DSN_SNAPSHOT_SETTINGS")) HK.open_settings();
    if (g_getenv("DSN_SCRIPT")) run_script(g_getenv("DSN_SCRIPT"));
    const char *q = g_getenv("DSN_QUIT_AFTER");
    if (q) g_timeout_add((guint)(g_ascii_strtod(q, NULL) * 1000), quit_cb, NULL);
}

#endif
