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
#include "testhooks.h"
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

/* ---------------- test hooks (debug builds only) ---------------- */

#ifdef DSN_TEST_HOOKS
static GtkWidget *hk_window(void) { return A.window; }
static void hk_open_settings(void) { prefs_show(GTK_WINDOW(A.window), on_prefs_changed, NULL); }
static void hk_quit(void) {
    save_window_state();
    gtk_widget_destroy(A.window);
}
#endif

/* ---------------- startup ---------------- */

static void key_resolved(gpointer p) {
    if (!api_key_present() && !testhooks_suppress_key_prompt()) {
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
    int ww = st->win_w, wh = st->win_h;
#ifdef DSN_TEST_HOOKS
    testhooks_window_size(&ww, &wh);
#endif
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
#ifdef DSN_TEST_HOOKS
    TestHooksCtx hk = { hk_window, A.chat, open_session, new_session, hk_open_settings, hk_quit, on_agent_event };
    testhooks_run(&hk);
#endif
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
