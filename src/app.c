#include "app.h"
#include "chat.h"
#include "sidebar.h"
#include "prefs.h"
#include "store.h"
#include "agent.h"
#include "theme.h"
#include "process.h"
#include "transcript.h"
#include "importer.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *header;
    GtkWidget *paned;
    GtkWidget *sidebar_toggle;
    Sidebar *sidebar;
    Chat *chat;
    GtkCssProvider *css;
    GSettings *iface;
    guint snapshot_source;
    int snapshot_n;
} App;

static App A;

static void update_title(void);
static void new_session(const char *ws);
static void pick_workspace(void);

/* ---------------- theme & css ---------------- */

static void build_css(void) {
    GString *c = g_string_new("");
#define COL(id) rgba_css(th(id))
    char *bg = COL(C_BG), *side = COL(C_SIDEBAR_BG), *sep = COL(C_SEPARATOR), *card = COL(C_CARD_BG), *cardb = COL(C_CARD_BORDER),
         *text = COL(C_TEXT), *t2 = COL(C_TEXT2), *t3 = COL(C_TEXT3), *t4 = COL(C_TEXT4), *acc = COL(C_ACCENT), *accs = COL(C_ACCENT_SOFT),
         *hover = COL(C_HOVER), *pressed = COL(C_PRESSED), *warn = COL(C_WARNING);
#undef COL
    gboolean dark = theme_dark();
    g_string_append_printf(c,
        ".chat, transcript { background-color: %s; }\n"
        ".sidebar { background-color: %s; border-right: 1px solid %s; }\n"
        ".sidebar .session-list, .sidebar scrolledwindow, .sidebar viewport { background-color: transparent; }\n"
        ".session-list row { border-radius: 8px; margin: 1px 8px; padding: 0; background-color: transparent; color: %s; }\n"
        ".session-list row:hover { background-color: %s; }\n"
        ".session-list row:selected, .session-list row:selected:hover { background-color: %s; color: %s; }\n"
        ".session-list row.workspace-row { margin-top: 4px; }\n"
        ".accent-icon { color: %s; }\n"
        ".brand { font-weight: 600; }\n"
        ".brand-sub { font-family: monospace; font-size: 9px; color: %s; }\n"
        "button.new-session { border-radius: 12px; border: 1px solid %s; background-image: none; background-color: %s; min-height: 30px; box-shadow: none; color: %s; }\n"
        "button.new-session:hover { background-color: %s; }\n"
        ".section-title, .caption { font-size: smaller; }\n"
        ".caption-heading { font-size: smaller; font-weight: bold; }\n"
        ".composer { background-color: %s; border: 1px solid %s; border-radius: 22px; box-shadow: 0 2px 6px rgba(0,0,0,%s), 0 1px 2px rgba(0,0,0,%s); }\n"
        ".composer-text, .composer-text text { background-color: transparent; color: %s; caret-color: %s; }\n"
        ".composer .placeholder { color: %s; }\n"
        ".composer button.pill { padding: 2px 8px; min-height: 26px; border-radius: 10px; color: %s; background-image: none; box-shadow: none; border: none; }\n"
        ".composer button.pill:hover { background-color: %s; }\n"
        ".composer button.pill.warning, .composer button.pill.warning image { color: %s; }\n"
        ".composer button.circular { color: %s; }\n"
        "button.chip { padding: 1px 8px; border-radius: 9px; min-height: 22px; font-size: smaller; }\n"
        ".status-line { color: %s; font-size: smaller; }\n"
        ".empty-title { font-size: 26px; font-weight: 600; color: %s; }\n"
        ".badge { color: %s; background-color: %s; border-radius: 9px; padding: 1px 9px; font-family: monospace; font-size: 11px; font-weight: 500; }\n"
        "button.workspace-button { color: %s; }\n"
        "button.scroll-down { background-image: none; background-color: %s; border: 1px solid %s; box-shadow: 0 2px 8px rgba(0,0,0,0.15); color: %s; }\n"
        "popover.menu-popover button.menu-row { padding: 5px 12px; border-radius: 8px; margin: 0 4px; }\n"
        ".prefs-section { font-weight: bold; }\n",
        bg, side, sep, text, hover, pressed, text, acc, t3, cardb, card, text, hover, card, cardb, dark ? "0.35" : "0.10", dark ? "0.3" : "0.06",
        text, acc, t4, t2, hover, warn, t2, t3, text, acc, accs, t2, card, cardb, t2);
    gtk_css_provider_load_from_data(A.css, c->str, -1, NULL);
    g_string_free(c, TRUE);
    g_free(bg); g_free(side); g_free(sep); g_free(card); g_free(cardb); g_free(text); g_free(t2); g_free(t3); g_free(t4);
    g_free(acc); g_free(accs); g_free(hover); g_free(pressed); g_free(warn);
}

static void apply_appearance(void) {
    GtkSettings *gs = gtk_settings_get_default();
    gboolean dark;
    switch (settings()->appearance) {
    case APPEAR_LIGHT: dark = FALSE; break;
    case APPEAR_DARK: dark = TRUE; break;
    default: {
        dark = FALSE;
        if (A.iface) {
            char *scheme = g_settings_get_string(A.iface, "color-scheme");
            dark = scheme && !strcmp(scheme, "prefer-dark");
            g_free(scheme);
        }
        break;
    }
    }
    const char *env = g_getenv("DSN_APPEARANCE");
    if (env) dark = !strcmp(env, "dark");
    g_object_set(gs, "gtk-application-prefer-dark-theme", dark, NULL);
    theme_force_appearance(settings()->appearance == APPEAR_SYSTEM ? 0 : settings()->appearance == APPEAR_DARK ? 2 : 1);
}

static void refresh_theme(void) {
    if (!A.window) return;
    if (theme_update(A.window)) {
        build_css();
        if (A.chat) chat_theme_changed(A.chat);
        gtk_widget_queue_draw(A.window);
    }
}

static void on_style_updated(GtkWidget *w, gpointer p) { refresh_theme(); }
static void on_gtk_setting(GObject *o, GParamSpec *ps, gpointer p) { refresh_theme(); }

static void on_color_scheme(GSettings *s, const char *key, gpointer p) {
    if (settings()->appearance == APPEAR_SYSTEM) {
        apply_appearance();
        refresh_theme();
    }
}

static void on_prefs_changed(gpointer p) {
    apply_appearance();
    refresh_theme();
    if (A.chat) chat_theme_changed(A.chat);
}

/* ---------------- sessions ---------------- */

static void open_session(const char *id) {
    Session *s = store_session(id);
    if (!s) return;
    chat_set_pending_workspace(A.chat, s->cwd);
    chat_show(A.chat, s);
    sidebar_select(A.sidebar, id);
    Workspace *w = store_workspace_for_path(s->cwd);
    if (w) store_set_last_workspace(w->id);
    store_trim(id);
    update_title();
}

static void new_session(const char *ws) {
    const char *target = ws;
    Session *cur = chat_session(A.chat);
    if (!target) target = cur ? cur->cwd : chat_pending_workspace(A.chat);
    if (!target && store_workspaces()->len) target = ((Workspace *)g_ptr_array_index(store_workspaces(), 0))->path;
    if (!target) { pick_workspace(); return; }
    char *t = g_strdup(target);
    chat_set_pending_workspace(A.chat, t);
    chat_show(A.chat, NULL);
    sidebar_select(A.sidebar, NULL);
    update_title();
    chat_focus(A.chat);
    g_free(t);
}

static void pick_workspace(void) {
    GtkFileChooserNative *fc = gtk_file_chooser_native_new("Choose Workspace", GTK_WINDOW(A.window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Choose", "_Cancel");
    const char *cur = chat_workspace(A.chat);
    if (cur) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), cur);
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        Workspace *w = store_add_workspace(path);
        store_set_last_workspace(w->id);
        new_session(w->path);
        g_free(path);
    }
    g_object_unref(fc);
}

static void rename_session(const char *id) {
    IndexEntry *e = store_entry(id);
    if (!e) return;
    GtkWidget *d = gtk_dialog_new_with_buttons("Rename Session", GTK_WINDOW(A.window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_USE_HEADER_BAR,
                                               "_Cancel", GTK_RESPONSE_CANCEL, "_Rename", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), e->title);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 36);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_set_border_width(GTK_CONTAINER(area), 16);
    gtk_container_add(GTK_CONTAINER(area), entry);
    gtk_widget_show_all(d);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *t = str_trim(gtk_entry_get_text(GTK_ENTRY(entry)));
        if (*t) store_rename(id, t);
        g_free(t);
        update_title();
    }
    gtk_widget_destroy(d);
}

static void delete_session(const char *id) {
    IndexEntry *e = store_entry(id);
    if (!e) return;
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(A.window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                          "Delete “%s”?", e->title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "The session transcript will be permanently removed.");
    gtk_dialog_add_button(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL);
    GtkWidget *del = gtk_dialog_add_button(GTK_DIALOG(d), "_Delete", GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class(gtk_widget_get_style_context(del), "destructive-action");
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (r != GTK_RESPONSE_ACCEPT) return;
    char *sid = g_strdup(id);
    char *cwd = g_strdup(e->cwd);
    Session *cur = chat_session(A.chat);
    gboolean showing = cur && !strcmp(cur->id, sid);
    if (showing) new_session(cwd);
    store_delete(sid);
    g_free(sid);
    g_free(cwd);
}

static void update_title(void) {
    Session *s = chat_session(A.chat);
    if (s && s->messages->len) {
        gtk_window_set_title(GTK_WINDOW(A.window), s->title);
        gtk_header_bar_set_title(GTK_HEADER_BAR(A.header), s->title);
        char *t = tilde_path(s->cwd);
        char *sub = s->running ? g_strconcat(t, "  ·  Working…", NULL) : g_strdup(t);
        gtk_header_bar_set_subtitle(GTK_HEADER_BAR(A.header), sub);
        g_free(sub);
        g_free(t);
    } else {
        gtk_window_set_title(GTK_WINDOW(A.window), "DeepSeek");
        gtk_header_bar_set_title(GTK_HEADER_BAR(A.header), "New Session");
        const char *ws = chat_pending_workspace(A.chat);
        char *t = ws ? tilde_path(ws) : g_strdup("");
        gtk_header_bar_set_subtitle(GTK_HEADER_BAR(A.header), t);
        g_free(t);
    }
}

/* ---------------- callbacks ---------------- */

static void sb_select(const char *id, gpointer d) { open_session(id); }
static void sb_new(const char *ws, gpointer d) { new_session(ws); }
static void sb_add_ws(gpointer d) { pick_workspace(); }
static void sb_settings(gpointer d) { prefs_show(GTK_WINDOW(A.window), on_prefs_changed, NULL); }
static void sb_rename(const char *id, gpointer d) { rename_session(id); }
static void sb_delete(const char *id, gpointer d) { delete_session(id); }

static void ch_started(Session *s, gpointer d) {
    sidebar_select(A.sidebar, s->id);
    Workspace *w = store_workspace_for_path(s->cwd);
    if (w) store_set_last_workspace(w->id);
    update_title();
}
static void ch_new(gpointer d) { new_session(NULL); }
static void ch_ws(gpointer d) { pick_workspace(); }
static void ch_settings(gpointer d) { sb_settings(NULL); }
static void ch_rename(gpointer d) {
    Session *s = chat_session(A.chat);
    if (s && store_entry(s->id)) rename_session(s->id);
}
static void ch_meta(gpointer d) { update_title(); }

static gboolean trim_idle(gpointer p) {
    if (A.window) {
        Session *cur = chat_session(A.chat);
        store_trim(cur ? cur->id : NULL);
    }
    return G_SOURCE_REMOVE;
}

static void on_agent_event(Session *s, gboolean attention, gpointer d) {
    if (!A.window || s->ephemeral) return;
    /* Trimming may free `s`; defer it until this callback chain has finished with the session. */
    if (!attention && s != chat_session(A.chat)) g_idle_add(trim_idle, NULL);
    update_title();
    if (gtk_window_is_active(GTK_WINDOW(A.window)) || g_getenv("DSN_NO_OPEN")) return;
    gtk_window_set_urgency_hint(GTK_WINDOW(A.window), TRUE);
    GNotification *n = g_notification_new(attention ? "DeepSeek needs your input" : "DeepSeek finished");
    g_notification_set_body(n, s->title);
    GIcon *icon = g_themed_icon_new("io.github.lszl84.DeepSeekNative");
    g_notification_set_icon(n, icon);
    g_object_unref(icon);
    g_application_send_notification(G_APPLICATION(A.app), "turn", n);
    g_object_unref(n);
}

static gboolean on_focus_in(GtkWidget *w, GdkEvent *e, gpointer p) {
    gtk_window_set_urgency_hint(GTK_WINDOW(w), FALSE);
    g_application_withdraw_notification(G_APPLICATION(A.app), "turn");
    return FALSE;
}

/* ---------------- actions ---------------- */

static void a_new(GSimpleAction *a, GVariant *v, gpointer p) { new_session(NULL); }
static void a_open_ws(GSimpleAction *a, GVariant *v, gpointer p) { pick_workspace(); }
static void a_stop(GSimpleAction *a, GVariant *v, gpointer p) { chat_stop(A.chat); }
static void a_retry(GSimpleAction *a, GVariant *v, gpointer p) { chat_retry(A.chat); }
static void a_focus(GSimpleAction *a, GVariant *v, gpointer p) { chat_focus(A.chat); }
static void a_copy_last(GSimpleAction *a, GVariant *v, gpointer p) { chat_copy_last(A.chat); }
static void a_export(GSimpleAction *a, GVariant *v, gpointer p) { chat_export(A.chat); }
static void a_settings(GSimpleAction *a, GVariant *v, gpointer p) { sb_settings(NULL); }
static void a_rename(GSimpleAction *a, GVariant *v, gpointer p) { ch_rename(NULL); }
static void a_search(GSimpleAction *a, GVariant *v, gpointer p) {
    if (!gtk_widget_get_visible(sidebar_widget(A.sidebar))) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.sidebar_toggle), TRUE);
    sidebar_focus_search(A.sidebar);
}
static void a_delete(GSimpleAction *a, GVariant *v, gpointer p) {
    Session *s = chat_session(A.chat);
    if (s && store_entry(s->id)) delete_session(s->id);
}
static void a_toggle_sidebar(GSimpleAction *a, GVariant *v, gpointer p) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.sidebar_toggle), !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.sidebar_toggle)));
}
static void a_reveal_ws(GSimpleAction *a, GVariant *v, gpointer p) {
    const char *ws = chat_workspace(A.chat);
    if (!ws) return;
    char *u = g_filename_to_uri(ws, NULL, NULL);
    ui_show_uri(A.window, u);
    g_free(u);
}
static void a_terminal(GSimpleAction *a, GVariant *v, gpointer p) {
    const char *ws = chat_workspace(A.chat);
    if (ws) open_in_terminal(ws);
}
static void a_reveal_log(GSimpleAction *a, GVariant *v, gpointer p) {
    Session *s = chat_session(A.chat);
    if (!s) return;
    char *u = g_filename_to_uri(s->file_path, NULL, NULL);
    ui_show_uri(A.window, u);
    g_free(u);
}

static gboolean confirm_quit(void) {
    GPtrArray *running = store_running_sessions();
    guint n = running->len;
    if (n == 0) { g_ptr_array_unref(running); return TRUE; }
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(A.window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                          "%u session%s still working", n, n == 1 ? " is" : "s are");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "Quitting stops the running agents.");
    gtk_dialog_add_button(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(d), "_Quit", GTK_RESPONSE_ACCEPT);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (r == GTK_RESPONSE_ACCEPT) {
        for (guint i = 0; i < running->len; i++) {
            Session *s = g_ptr_array_index(running, i);
            if (s->agent) { agent_cancel(s->agent); jobs_kill_all(s->agent->jobs); }
        }
    }
    g_ptr_array_unref(running);
    return r == GTK_RESPONSE_ACCEPT;
}

static void save_window_state(void) {
    if (!A.window) return;
    int w, h;
    gtk_window_get_size(GTK_WINDOW(A.window), &w, &h);
    Settings *s = settings();
    if (!gtk_window_is_maximized(GTK_WINDOW(A.window))) { s->win_w = w; s->win_h = h; }
    s->sidebar_w = gtk_paned_get_position(GTK_PANED(A.paned));
    s->sidebar_visible = gtk_widget_get_visible(sidebar_widget(A.sidebar));
    settings_save();
    store_flush();
}

static void a_quit(GSimpleAction *a, GVariant *v, gpointer p) {
    if (!confirm_quit()) return;
    save_window_state();
    gtk_widget_destroy(A.window);
}

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer p) {
    if (!confirm_quit()) return TRUE;
    save_window_state();
    return FALSE;
}

static void sidebar_toggled(GtkToggleButton *b, gpointer p) { gtk_widget_set_visible(sidebar_widget(A.sidebar), gtk_toggle_button_get_active(b)); }


/* ---------------- Harness import ---------------- */

typedef struct { int count; gboolean report; } ImportResult;

static void import_done(gpointer p) {
    ImportResult *r = p;
    if (r->report && A.window) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(A.window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                              r->count == 0 ? "No new sessions to import" : r->count == 1 ? "Imported 1 session" : "Imported %d sessions", r->count);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "Sessions are read from ~/.dsh/sessions. Existing imports are skipped.");
        g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
        gtk_widget_show(d);
    }
    g_free(r);
}

static gpointer import_thread(gpointer p) {
    ImportResult *r = p;
    r->count = harness_import_all();
    main_async(import_done, r);
    return NULL;
}

static void start_import(gboolean report) {
    ImportResult *r = g_new0(ImportResult, 1);
    r->report = report;
    g_thread_unref(g_thread_new("import", import_thread, r));
}

static void a_import(GSimpleAction *a, GVariant *v, gpointer p) { start_import(TRUE); }

/* ---------------- debug hooks ---------------- */

static gboolean snapshot_tick(gpointer p) {
    const char *path = g_getenv("DSN_SNAPSHOT");
    if (!path || !A.window) return G_SOURCE_REMOVE;
    A.snapshot_n++;
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
        char *fn = w == A.window ? g_strdup_printf("%s-%d.png", path, A.snapshot_n) : g_strdup_printf("%s-%d-w%d.png", path, A.snapshot_n, k++);
        cairo_surface_write_to_png(s, fn);
        g_free(fn);
        cairo_surface_destroy(s);
    }
    g_list_free(tops);
    GtkWidget *pops[4];
    int np = 0;
    chat_debug_popovers(A.chat, pops, &np);
    for (int i = 0; i < np; i++) {
        int W = gtk_widget_get_allocated_width(pops[i]), H = gtk_widget_get_allocated_height(pops[i]);
        if (W < 2 || H < 2) continue;
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        cairo_t *cr = cairo_create(s);
        gtk_widget_draw(pops[i], cr);
        cairo_destroy(cr);
        char *fn = g_strdup_printf("%s-%d-pop%d.png", path, A.snapshot_n, i);
        cairo_surface_write_to_png(s, fn);
        g_free(fn);
        cairo_surface_destroy(s);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean autosend_cb(gpointer p) {
    const char *t = g_getenv("DSN_AUTOSEND");
    if (t) chat_send_text(A.chat, t);
    return G_SOURCE_REMOVE;
}

static gboolean compose_cb(gpointer p) {
    chat_compose(A.chat, g_getenv("DSN_COMPOSE"));
    return G_SOURCE_REMOVE;
}

static gboolean expand_cb(gpointer p) {
    chat_expand_all(A.chat);
    return G_SOURCE_REMOVE;
}

static gboolean quit_cb(gpointer p) {
    GPtrArray *running = store_running_sessions();
    for (guint i = 0; i < running->len; i++) {
        Session *s = g_ptr_array_index(running, i);
        if (s->agent) agent_cancel(s->agent);
    }
    g_ptr_array_unref(running);
    save_window_state();
    gtk_widget_destroy(A.window);
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
    on_agent_event(s, attention, d);
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
    GdkWindow *top = gtk_widget_get_window(A.window);
    int lx, ly;
    GdkWindow *target = window_at(top, x, y, &lx, &ly);
    gpointer ud = NULL;
    gdk_window_get_user_data(target, &ud);
    dlog("synth button %d at %d,%d -> %s (%d,%d)", type, x, y, ud ? G_OBJECT_TYPE_NAME(ud) : "?", lx, ly);
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
    GdkWindow *top = gtk_widget_get_window(A.window);
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
    GtkWindow *w = GTK_WINDOW(A.window);
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
        GtkWidget *tw = chat_transcript(A.chat);
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
        Session *cur = chat_session(A.chat);
        if (cur) {
            char *id = g_strdup(cur->id), *cwd = g_strdup(cur->cwd);
            new_session(cwd);
            store_delete(id);
            g_free(id);
            g_free(cwd);
        }
    } else if (!strcmp(cmd, "stats")) {
        GPtrArray *running = store_running_sessions();
        fprintf(stderr, "[stats] running=%u indexed=%u\n", running->len, store_index()->len);
        g_ptr_array_unref(running);
    } else if (g_str_has_prefix(cmd, "scroll ")) {
        ds_transcript_scroll_to(DS_TRANSCRIPT(chat_transcript(A.chat)), g_ascii_strtod(cmd + 7, NULL));
    } else if (g_str_has_prefix(cmd, "doc ")) {
        ds_transcript_render_document(DS_TRANSCRIPT(chat_transcript(A.chat)), cmd + 4);
    } else if (g_str_has_prefix(cmd, "expand")) {
        chat_expand_all(A.chat);
    } else if (g_str_has_prefix(cmd, "snap ")) {
        const char *base = g_getenv("DSN_SNAPSHOT");
        char *old = g_strdup(base ? base : "/tmp/dsn-snap");
        g_setenv("DSN_SNAPSHOT", cmd + 5, TRUE);
        int n = A.snapshot_n;
        A.snapshot_n = -1;
        snapshot_tick(NULL);
        A.snapshot_n = n;
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

static void run_debug_hooks(void) {
    const char *open = g_getenv("DSN_OPEN");
    if (open) {
        open_session(open);
        if (g_getenv("DSN_EXPAND_ALL")) g_timeout_add(500, expand_cb, NULL);
    }
    const char *perm = g_getenv("DSN_PERMISSION");
    if (perm) settings()->permission = perm_parse(perm);
    const char *ws = g_getenv("DSN_WORKSPACE");
    if (ws) {
        Workspace *w = store_add_workspace(ws);
        store_set_last_workspace(w->id);
        new_session(w->path);
    }
    if (g_getenv("DSN_AUTOSEND")) g_timeout_add(1000, autosend_cb, NULL);
    if (g_getenv("DSN_COMPOSE")) g_timeout_add(1000, compose_cb, NULL);
    if (g_getenv("DSN_AUTOANSWER")) agent_set_event_handler(autoanswer_event, NULL);
    if (g_getenv("DSN_SNAPSHOT")) {
        double iv = g_getenv("DSN_SNAPSHOT_INTERVAL") ? g_ascii_strtod(g_getenv("DSN_SNAPSHOT_INTERVAL"), NULL) : 5;
        A.snapshot_source = g_timeout_add((guint)(iv * 1000), snapshot_tick, NULL);
    }
    if (g_getenv("DSN_SNAPSHOT_SETTINGS")) prefs_show(GTK_WINDOW(A.window), on_prefs_changed, NULL);
    if (g_getenv("DSN_SCRIPT")) run_script(g_getenv("DSN_SCRIPT"));
    const char *q = g_getenv("DSN_QUIT_AFTER");
    if (q) g_timeout_add((guint)(g_ascii_strtod(q, NULL) * 1000), quit_cb, NULL);
}

/* ---------------- startup ---------------- */

static void key_resolved(gpointer p) {
    if (!api_key_present() && !g_getenv("DSN_NO_KEY_PROMPT")) {
        g_setenv("DSN_SETTINGS_PAGE", "models", FALSE);
        prefs_show(GTK_WINDOW(A.window), on_prefs_changed, NULL);
    }
}

static GtkWidget *header_button(const char *icon, const char *tip, const char *action) {
    GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    return b;
}

static void activate(GtkApplication *app, gpointer p) {
    if (A.window) { gtk_window_present(GTK_WINDOW(A.window)); return; }
    shell_env_warm_up();
    store_init();
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema = src ? g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE) : NULL;
    if (schema) {
        if (g_settings_schema_has_key(schema, "color-scheme")) {
            A.iface = g_settings_new("org.gnome.desktop.interface");
            g_signal_connect(A.iface, "changed::color-scheme", G_CALLBACK(on_color_scheme), NULL);
        }
        g_settings_schema_unref(schema);
    }
    apply_appearance();

    A.window = gtk_application_window_new(app);
    Settings *st = settings();
    const char *size = g_getenv("DSN_WINDOW_SIZE");
    int ww = st->win_w, wh = st->win_h;
    if (size) sscanf(size, "%dx%d", &ww, &wh);
    gtk_window_set_default_size(GTK_WINDOW(A.window), MAX(720, ww), MAX(480, wh));
    if (gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), "io.github.lszl84.DeepSeekNative"))
        gtk_window_set_icon_name(GTK_WINDOW(A.window), "io.github.lszl84.DeepSeekNative");
    else {
        GdkPixbuf *icon = theme_logo_pixbuf(128, (RGBA){ 0.30, 0.42, 0.996, 1 });
        gtk_window_set_icon(GTK_WINDOW(A.window), icon);
        g_object_unref(icon);
    }
    A.css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(A.css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    theme_update(A.window);
    theme_set_pango(gtk_widget_get_pango_context(A.window));
    theme_set_scale(gtk_widget_get_scale_factor(A.window));
    build_css();

    A.header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(A.header), TRUE);
    gtk_header_bar_set_has_subtitle(GTK_HEADER_BAR(A.header), TRUE);
    A.sidebar_toggle = gtk_toggle_button_new();
    gtk_container_add(GTK_CONTAINER(A.sidebar_toggle), gtk_image_new_from_icon_name("sidebar-show-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text(A.sidebar_toggle, "Toggle Sidebar (F9)");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(A.header), A.sidebar_toggle);
    GMenu *menu = g_menu_new();
    GMenu *s1 = g_menu_new();
    g_menu_append(s1, "Rename Session…", "app.rename");
    g_menu_append(s1, "Export as Markdown…", "app.export");
    g_menu_append(s1, "Copy Last Response", "app.copy-last");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s1));
    GMenu *s2 = g_menu_new();
    g_menu_append(s2, "Open Workspace…", "app.open-workspace");
    g_menu_append(s2, "Show Workspace in Files", "app.reveal-workspace");
    g_menu_append(s2, "Open Workspace in Terminal", "app.terminal");
    g_menu_append(s2, "Open Session Log", "app.reveal-log");
    g_menu_append(s2, "Import DeepSeek Harness Sessions", "app.import");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s2));
    GMenu *s3 = g_menu_new();
    g_menu_append(s3, "Delete Session…", "app.delete");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s3));
    GMenu *s4 = g_menu_new();
    g_menu_append(s4, "Settings", "app.settings");
    g_menu_append(s4, "Keyboard Shortcuts", "app.shortcuts");
    g_menu_append(s4, "Quit", "app.quit");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s4));
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), G_MENU_MODEL(menu));
    gtk_container_add(GTK_CONTAINER(mb), gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text(mb, "Menu");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(A.header), mb);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(A.header), header_button("document-edit-symbolic", "New Session (Ctrl+N)", "app.new"));
    g_object_unref(menu);
    g_object_unref(s1);
    g_object_unref(s2);
    g_object_unref(s3);
    g_object_unref(s4);
    gtk_window_set_titlebar(GTK_WINDOW(A.window), A.header);

    SidebarCallbacks scb = { sb_select, sb_new, sb_add_ws, sb_settings, sb_rename, sb_delete, NULL };
    A.sidebar = sidebar_new(scb);
    ChatCallbacks ccb = { ch_started, ch_new, ch_ws, ch_settings, ch_rename, ch_meta, NULL };
    A.chat = chat_new(ccb);
    A.paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(A.paned), sidebar_widget(A.sidebar), FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(A.paned), chat_widget(A.chat), TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(A.paned), CLAMP(st->sidebar_w, 220, 380));
    gtk_container_add(GTK_CONTAINER(A.window), A.paned);
    g_signal_connect(A.sidebar_toggle, "toggled", G_CALLBACK(sidebar_toggled), NULL);

    agent_set_event_handler(on_agent_event, NULL);
    g_signal_connect(A.window, "style-updated", G_CALLBACK(on_style_updated), NULL);
    GtkSettings *gs = gtk_settings_get_default();
    g_signal_connect(gs, "notify::gtk-theme-name", G_CALLBACK(on_gtk_setting), NULL);
    g_signal_connect(gs, "notify::gtk-font-name", G_CALLBACK(on_gtk_setting), NULL);
    g_signal_connect(gs, "notify::gtk-application-prefer-dark-theme", G_CALLBACK(on_gtk_setting), NULL);
    g_signal_connect(A.window, "focus-in-event", G_CALLBACK(on_focus_in), NULL);
    g_signal_connect(A.window, "delete-event", G_CALLBACK(on_delete), NULL);
    g_signal_connect(A.window, "destroy", G_CALLBACK(gtk_widget_destroyed), &A.window);

    gtk_widget_show_all(A.window);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.sidebar_toggle), st->sidebar_visible);
    gtk_widget_set_visible(sidebar_widget(A.sidebar), st->sidebar_visible);

    /* Restore the last workspace or session. */
    Workspace *w = store_workspace(store_last_workspace());
    if (!w && store_workspaces()->len) w = g_ptr_array_index(store_workspaces(), 0);
    if (w) {
        GPtrArray *ss = store_sessions_in(w, FALSE);
        if (ss->len) open_session(((IndexEntry *)g_ptr_array_index(ss, 0))->id);
        else {
            chat_set_pending_workspace(A.chat, w->path);
            chat_show(A.chat, NULL);
        }
        g_ptr_array_unref(ss);
    } else chat_show(A.chat, NULL);
    update_title();
    chat_focus(A.chat);
    run_debug_hooks();
    api_key_resolve_async(key_resolved, NULL);
    if (store_index()->len == 0) {
        GPtrArray *cands = harness_candidates();
        if (cands->len) start_import(FALSE);
        g_ptr_array_unref(cands);
    }
}

static void a_shortcuts(GSimpleAction *a, GVariant *v, gpointer p) {
    const char *rows[][2] = {
        { "Ctrl+N", "New session" }, { "Ctrl+O", "Open workspace" }, { "Ctrl+.", "Stop the running turn" }, { "Ctrl+R", "Retry last turn" },
        { "Ctrl+L", "Focus message box" }, { "Ctrl+K", "Search sessions" }, { "Ctrl+Shift+C", "Copy last response" },
        { "Ctrl+Shift+E", "Export as Markdown" }, { "F2", "Rename session" }, { "F9", "Toggle sidebar" }, { "Ctrl+,", "Settings" },
        { "Ctrl+Q", "Quit" }, { "Enter / Shift+Enter", "Send / new line" },
    };
    GtkWidget *d = gtk_dialog_new_with_buttons("Keyboard Shortcuts", GTK_WINDOW(A.window), GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_USE_HEADER_BAR, NULL, NULL);
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 8);
    gtk_grid_set_column_spacing(GTK_GRID(g), 28);
    gtk_container_set_border_width(GTK_CONTAINER(g), 20);
    for (size_t i = 0; i < G_N_ELEMENTS(rows); i++) {
        GtkWidget *k = gtk_label_new(rows[i][0]);
        gtk_label_set_xalign(GTK_LABEL(k), 1);
        gtk_style_context_add_class(gtk_widget_get_style_context(k), "dim-label");
        GtkWidget *l = gtk_label_new(rows[i][1]);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_grid_attach(GTK_GRID(g), k, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(g), l, 1, i, 1, 1);
    }
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))), g);
    g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show_all(d);
}

static void startup(GApplication *app, gpointer p) {
    const GActionEntry entries[] = {
        { "new", a_new }, { "open-workspace", a_open_ws }, { "stop", a_stop }, { "retry", a_retry }, { "focus", a_focus },
        { "copy-last", a_copy_last }, { "export", a_export }, { "settings", a_settings }, { "rename", a_rename }, { "delete", a_delete },
        { "toggle-sidebar", a_toggle_sidebar }, { "reveal-workspace", a_reveal_ws }, { "terminal", a_terminal }, { "reveal-log", a_reveal_log },
        { "quit", a_quit }, { "search", a_search }, { "shortcuts", a_shortcuts }, { "import", a_import },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), entries, G_N_ELEMENTS(entries), NULL);
    struct { const char *action; const char *accels[3]; } accels[] = {
        { "app.new", { "<Control>n", NULL } },
        { "app.open-workspace", { "<Control>o", NULL } },
        { "app.stop", { "<Control>period", NULL } },
        { "app.retry", { "<Control>r", NULL } },
        { "app.focus", { "<Control>l", NULL } },
        { "app.copy-last", { "<Control><Shift>c", NULL } },
        { "app.export", { "<Control><Shift>e", NULL } },
        { "app.settings", { "<Control>comma", NULL } },
        { "app.rename", { "F2", NULL } },
        { "app.toggle-sidebar", { "F9", "<Control><Shift>s", NULL } },
        { "app.quit", { "<Control>q", NULL } },
        { "app.search", { "<Control>k", NULL } },
        { "app.shortcuts", { "<Control>question", NULL } },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(accels); i++) gtk_application_set_accels_for_action(GTK_APPLICATION(app), accels[i].action, accels[i].accels);
}

int app_run(int argc, char **argv) {
    g_set_application_name("DeepSeek");
    g_set_prgname("deepseek-native");
    GApplicationFlags flags = G_APPLICATION_DEFAULT_FLAGS;
    if (g_getenv("DSN_DATA_DIR")) flags |= G_APPLICATION_NON_UNIQUE;
    A.app = gtk_application_new("io.github.lszl84.DeepSeekNative", flags);
    g_signal_connect(A.app, "startup", G_CALLBACK(startup), NULL);
    g_signal_connect(A.app, "activate", G_CALLBACK(activate), NULL);
    int r = g_application_run(G_APPLICATION(A.app), argc, argv);
    store_flush();
    g_object_unref(A.app);
    return r;
}
