#include "sidebar.h"
#include "store.h"
#include "theme.h"
#include <string.h>

typedef struct {
    GtkWidget *row;
    GtkWidget *title;
    GtkWidget *time;
    GtkWidget *spinner;
    GtkWidget *dot;
    GtkWidget *pin;
} SessionRow;

struct Sidebar {
    SidebarCallbacks cb;
    GtkWidget *root;
    GtkWidget *search;
    GtkWidget *list;
    GHashTable *rows; /* session id -> SessionRow* */
    char *selected;
    gboolean suppress;
    guint reload_source;
    guint clock_source;
    gboolean dead;
};

GtkWidget *sidebar_widget(Sidebar *s) { return s->root; }

static gboolean logo_draw(GtkWidget *w, cairo_t *cr, gpointer p) {
    double sz = MIN(gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
    draw_logo(cr, 0, (gtk_widget_get_allocated_height(w) - sz) / 2, sz, th(C_ACCENT));
    return TRUE;
}

static gboolean dot_draw(GtkWidget *w, cairo_t *cr, gpointer p) {
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    set_color(cr, C_WARNING);
    cairo_new_path(cr);
    cairo_arc(cr, W / 2, H / 2, 3.5, 0, 2 * G_PI);
    cairo_fill(cr);
    return TRUE;
}

static void update_row(Sidebar *sb, const char *id) {
    SessionRow *r = g_hash_table_lookup(sb->rows, id);
    IndexEntry *e = store_entry(id);
    if (!r || !e) return;
    gtk_label_set_text(GTK_LABEL(r->title), e->title);
    gtk_widget_set_tooltip_text(r->row, e->title);
    Session *live = store_live(id);
    gboolean running = live && live->running;
    gboolean attention = running && (live->pending_approvals->len || live->pending_questions->len);
    char *t = fmt_relative(e->updated_at);
    gtk_label_set_text(GTK_LABEL(r->time), t);
    g_free(t);
    gtk_widget_set_visible(r->time, !running);
    gtk_widget_set_visible(r->spinner, running);
    if (running) gtk_spinner_start(GTK_SPINNER(r->spinner));
    else gtk_spinner_stop(GTK_SPINNER(r->spinner));
    gtk_widget_set_visible(r->dot, attention);
    gtk_widget_set_visible(r->pin, e->pinned);
}

/* ---------------- context menus ---------------- */

static void menu_item(GtkWidget *menu, const char *label, GCallback cb, gpointer data, const char *arg) {
    GtkWidget *mi = gtk_menu_item_new_with_mnemonic(label);
    if (arg) g_object_set_data_full(G_OBJECT(mi), "arg", g_strdup(arg), g_free);
    g_signal_connect(mi, "activate", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

static const char *arg_of(GtkMenuItem *mi) { return g_object_get_data(G_OBJECT(mi), "arg"); }

static void m_rename(GtkMenuItem *mi, Sidebar *s) { if (s->cb.rename) s->cb.rename(arg_of(mi), s->cb.data); }
static void m_pin(GtkMenuItem *mi, Sidebar *s) {
    IndexEntry *e = store_entry(arg_of(mi));
    if (e) store_set_pinned(e->id, !e->pinned);
}
static void m_archive(GtkMenuItem *mi, Sidebar *s) { store_set_archived(arg_of(mi), TRUE); }
static void m_unarchive(GtkMenuItem *mi, Sidebar *s) {
    char *id = g_strdup(arg_of(mi));
    store_set_archived(id, FALSE);
    if (s->cb.select_session) s->cb.select_session(id, s->cb.data);
    g_free(id);
}
static void m_delete(GtkMenuItem *mi, Sidebar *s) { if (s->cb.delete_session) s->cb.delete_session(arg_of(mi), s->cb.data); }

static void show_uri(GtkWidget *w, const char *path) {
    char *u = g_filename_to_uri(path, NULL, NULL);
    ui_show_uri(w, u);
    g_free(u);
}

static void m_reveal_log(GtkMenuItem *mi, Sidebar *s) {
    char *fn = g_strconcat(arg_of(mi), ".jsonl", NULL);
    char *p = g_build_filename(sessions_dir(), fn, NULL);
    GDBusProxy *fm = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.freedesktop.FileManager1",
                                                   "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1", NULL, NULL);
    gboolean ok = FALSE;
    if (fm) {
        char *uri = g_filename_to_uri(p, NULL, NULL);
        const char *uris[] = { uri, NULL };
        GVariant *r = g_dbus_proxy_call_sync(fm, "ShowItems", g_variant_new("(^ass)", uris, ""), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
        if (r) { ok = TRUE; g_variant_unref(r); }
        g_free(uri);
        g_object_unref(fm);
    }
    if (!ok) show_uri(s->root, sessions_dir());
    g_free(p);
    g_free(fn);
}

static void m_new_in(GtkMenuItem *mi, Sidebar *s) { if (s->cb.new_session) s->cb.new_session(arg_of(mi), s->cb.data); }
static void m_reveal_ws(GtkMenuItem *mi, Sidebar *s) { show_uri(s->root, arg_of(mi)); }
static void m_terminal(GtkMenuItem *mi, Sidebar *s) { open_in_terminal(arg_of(mi)); }
static void m_remove_ws(GtkMenuItem *mi, Sidebar *s) { store_remove_workspace(arg_of(mi)); }

void open_in_terminal(const char *path) {
    if (g_getenv("DSN_NO_OPEN")) return;
    const char *candidates[] = { g_getenv("TERMINAL"), "xdg-terminal-exec", "alacritty", "kitty", "foot", "ghostty", "wezterm", "gnome-terminal",
                                 "konsole", "xterm", NULL };
    for (int i = 0; i < (int)G_N_ELEMENTS(candidates) - 1; i++) {
        const char *c = candidates[i];
        if (!c || !*c) continue;
        char *exe = g_find_program_in_path(c);
        if (!exe) continue;
        char *argv[] = { exe, NULL };
        g_spawn_async(path, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
        g_free(exe);
        return;
    }
}

static void popup_session_menu(Sidebar *s, const char *id, GdkEvent *ev) {
    IndexEntry *e = store_entry(id);
    if (!e) return;
    GtkWidget *m = gtk_menu_new();
    menu_item(m, "_Rename…", G_CALLBACK(m_rename), s, id);
    menu_item(m, e->pinned ? "_Unpin" : "_Pin", G_CALLBACK(m_pin), s, id);
    menu_item(m, "_Archive", G_CALLBACK(m_archive), s, id);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    menu_item(m, "Show Session _Log", G_CALLBACK(m_reveal_log), s, id);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    menu_item(m, "_Delete…", G_CALLBACK(m_delete), s, id);
    gtk_widget_show_all(m);
    gtk_menu_attach_to_widget(GTK_MENU(m), s->root, NULL);
    g_signal_connect(m, "deactivate", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(m), ev);
}

static void popup_ws_menu(Sidebar *s, Workspace *w, GdkEvent *ev) {
    GtkWidget *m = gtk_menu_new();
    menu_item(m, "_New Session", G_CALLBACK(m_new_in), s, w->path);
    menu_item(m, "Open in _File Manager", G_CALLBACK(m_reveal_ws), s, w->path);
    menu_item(m, "Open in _Terminal", G_CALLBACK(m_terminal), s, w->path);
    GPtrArray *all = store_sessions_in(w, TRUE);
    GtkWidget *sub = NULL;
    for (guint i = 0; i < all->len; i++) {
        IndexEntry *e = g_ptr_array_index(all, i);
        if (!e->archived) continue;
        if (!sub) sub = gtk_menu_new();
        menu_item(sub, e->title, G_CALLBACK(m_unarchive), s, e->id);
    }
    g_ptr_array_unref(all);
    if (sub) {
        GtkWidget *mi = gtk_menu_item_new_with_label("Restore Archived");
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), sub);
        gtk_menu_shell_append(GTK_MENU_SHELL(m), mi);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    menu_item(m, "_Remove Workspace", G_CALLBACK(m_remove_ws), s, w->id);
    gtk_widget_show_all(m);
    gtk_menu_attach_to_widget(GTK_MENU(m), s->root, NULL);
    g_signal_connect(m, "deactivate", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(m), ev);
}

/* ---------------- rows ---------------- */

static gboolean row_button(GtkWidget *w, GdkEventButton *ev, Sidebar *s) {
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3) return FALSE;
    const char *sid = g_object_get_data(G_OBJECT(w), "session");
    const char *wid = g_object_get_data(G_OBJECT(w), "workspace");
    if (sid) popup_session_menu(s, sid, (GdkEvent *)ev);
    else if (wid) {
        Workspace *ws = store_workspace(wid);
        if (ws) popup_ws_menu(s, ws, (GdkEvent *)ev);
    }
    return TRUE;
}

static GtkWidget *make_ws_row(Sidebar *s, Workspace *w, gboolean expanded) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "workspace-row");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_box_pack_start(GTK_BOX(box), gtk_image_new_from_icon_name(expanded ? "pan-down-symbolic" : "pan-end-symbolic", GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    GtkWidget *icon = gtk_image_new_from_icon_name("folder-symbolic", GTK_ICON_SIZE_MENU);
    gtk_style_context_add_class(gtk_widget_get_style_context(icon), "accent-icon");
    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    GtkWidget *l = gtk_label_new(path_base(w->path));
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(row), box);
    gtk_widget_set_tooltip_text(row, w->path);
    g_object_set_data_full(G_OBJECT(row), "workspace", g_strdup(w->id), g_free);
    g_signal_connect(row, "button-press-event", G_CALLBACK(row_button), s);
    return row;
}

static void session_row_free(gpointer p) { g_free(p); }

static GtkWidget *make_session_row(Sidebar *s, IndexEntry *e) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "session-row");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(box, 30);
    gtk_widget_set_margin_end(box, 10);
    gtk_widget_set_margin_top(box, 5);
    gtk_widget_set_margin_bottom(box, 5);
    SessionRow *r = g_new0(SessionRow, 1);
    r->row = row;
    r->pin = gtk_image_new_from_icon_name("view-pin-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_no_show_all(r->pin, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(r->pin), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), r->pin, FALSE, FALSE, 0);
    r->title = gtk_label_new(e->title);
    gtk_label_set_ellipsize(GTK_LABEL(r->title), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(r->title), 0);
    gtk_box_pack_start(GTK_BOX(box), r->title, TRUE, TRUE, 0);
    r->dot = gtk_drawing_area_new();
    gtk_widget_set_size_request(r->dot, 9, 9);
    gtk_widget_set_valign(r->dot, GTK_ALIGN_CENTER);
    g_signal_connect(r->dot, "draw", G_CALLBACK(dot_draw), NULL);
    gtk_widget_set_no_show_all(r->dot, TRUE);
    gtk_box_pack_start(GTK_BOX(box), r->dot, FALSE, FALSE, 0);
    r->spinner = gtk_spinner_new();
    gtk_widget_set_no_show_all(r->spinner, TRUE);
    gtk_box_pack_start(GTK_BOX(box), r->spinner, FALSE, FALSE, 0);
    r->time = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(r->time), "dim-label");
    gtk_style_context_add_class(gtk_widget_get_style_context(r->time), "caption");
    gtk_widget_set_no_show_all(r->time, TRUE);
    gtk_box_pack_start(GTK_BOX(box), r->time, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row), box);
    g_object_set_data_full(G_OBJECT(row), "session", g_strdup(e->id), g_free);
    g_signal_connect(row, "button-press-event", G_CALLBACK(row_button), s);
    g_hash_table_insert(s->rows, g_strdup(e->id), r);
    return row;
}

static void restore_selection(Sidebar *s) {
    s->suppress = TRUE;
    SessionRow *r = s->selected ? g_hash_table_lookup(s->rows, s->selected) : NULL;
    if (r) gtk_list_box_select_row(GTK_LIST_BOX(s->list), GTK_LIST_BOX_ROW(r->row));
    else gtk_list_box_unselect_all(GTK_LIST_BOX(s->list));
    s->suppress = FALSE;
}

void sidebar_reload(Sidebar *s) {
    if (s->reload_source) { g_source_remove(s->reload_source); s->reload_source = 0; }
    GtkAdjustment *adj = gtk_list_box_get_adjustment(GTK_LIST_BOX(s->list));
    double v = adj ? gtk_adjustment_get_value(adj) : 0;
    s->suppress = TRUE;
    GList *ch = gtk_container_get_children(GTK_CONTAINER(s->list));
    for (GList *l = ch; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(ch);
    g_hash_table_remove_all(s->rows);
    char *q = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(s->search)), -1);
    char *qt = g_strstrip(q);
    GPtrArray *ws = store_workspaces();
    for (guint i = 0; i < ws->len; i++) {
        Workspace *w = g_ptr_array_index(ws, i);
        GPtrArray *sessions = store_sessions_in(w, FALSE);
        GPtrArray *shown = g_ptr_array_new();
        for (guint k = 0; k < sessions->len; k++) {
            IndexEntry *e = g_ptr_array_index(sessions, k);
            char *t = g_utf8_strdown(e->title, -1);
            if (!*qt || strstr(t, qt)) g_ptr_array_add(shown, e);
            g_free(t);
        }
        if (*qt && !shown->len) { g_ptr_array_unref(shown); g_ptr_array_unref(sessions); continue; }
        gboolean expanded = !w->collapsed || *qt;
        gtk_container_add(GTK_CONTAINER(s->list), make_ws_row(s, w, expanded));
        if (expanded)
            for (guint k = 0; k < shown->len; k++) gtk_container_add(GTK_CONTAINER(s->list), make_session_row(s, g_ptr_array_index(shown, k)));
        g_ptr_array_unref(shown);
        g_ptr_array_unref(sessions);
    }
    g_free(q);
    gtk_widget_show_all(s->list);
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init(&it, s->rows);
    while (g_hash_table_iter_next(&it, &k, NULL)) update_row(s, k);
    s->suppress = FALSE;
    restore_selection(s);
    if (adj) gtk_adjustment_set_value(adj, v);
}

static gboolean reload_idle(gpointer p) {
    Sidebar *s = p;
    s->reload_source = 0;
    sidebar_reload(s);
    return G_SOURCE_REMOVE;
}

static void on_store(StoreEvent ev, Session *sess, gpointer data) {
    Sidebar *s = data;
    if (s->dead) return;
    if (ev == STORE_CHANGED) {
        if (!s->reload_source) s->reload_source = g_idle_add(reload_idle, s);
    } else if (sess) {
        update_row(s, sess->id);
    }
}

static gboolean clock_tick(gpointer p) {
    Sidebar *s = p;
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init(&it, s->rows);
    while (g_hash_table_iter_next(&it, &k, NULL)) update_row(s, k);
    return G_SOURCE_CONTINUE;
}

void sidebar_select(Sidebar *s, const char *id) {
    char *n = g_strdup(id);
    g_free(s->selected);
    s->selected = n;
    restore_selection(s);
}

void sidebar_focus_search(Sidebar *s) { gtk_widget_grab_focus(s->search); }

static void row_activated(GtkListBox *lb, GtkListBoxRow *row, Sidebar *s) {
    const char *wid = g_object_get_data(G_OBJECT(row), "workspace");
    if (wid) {
        Workspace *w = store_workspace(wid);
        if (w) {
            store_set_collapsed(w->id, !w->collapsed);
            if (!s->reload_source) s->reload_source = g_idle_add(reload_idle, s);
        }
    }
}

static void row_selected(GtkListBox *lb, GtkListBoxRow *row, Sidebar *s) {
    if (s->suppress || !row) return;
    const char *sid = g_object_get_data(G_OBJECT(row), "session");
    if (!sid) return;
    char *id = g_strdup(sid);
    g_free(s->selected);
    s->selected = g_strdup(id);
    if (s->cb.select_session) s->cb.select_session(id, s->cb.data);
    g_free(id);
}

static void new_clicked(GtkButton *b, Sidebar *s) {
    IndexEntry *e = s->selected ? store_entry(s->selected) : NULL;
    if (s->cb.new_session) s->cb.new_session(e ? e->cwd : NULL, s->cb.data);
}

static void add_ws_clicked(GtkButton *b, Sidebar *s) { if (s->cb.add_workspace) s->cb.add_workspace(s->cb.data); }
static void settings_clicked(GtkButton *b, Sidebar *s) { if (s->cb.open_settings) s->cb.open_settings(s->cb.data); }
static void search_changed(GtkSearchEntry *e, Sidebar *s) { sidebar_reload(s); }

static void root_destroyed(GtkWidget *w, Sidebar *s) {
    s->dead = TRUE;
    if (s->reload_source) { g_source_remove(s->reload_source); s->reload_source = 0; }
    if (s->clock_source) { g_source_remove(s->clock_source); s->clock_source = 0; }
    g_hash_table_remove_all(s->rows);
}

Sidebar *sidebar_new(SidebarCallbacks cb) {
    Sidebar *s = g_new0(Sidebar, 1);
    s->cb = cb;
    s->rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, session_row_free);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(v), "sidebar");
    gtk_widget_set_size_request(v, 220, -1);
    s->root = v;

    GtkWidget *brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(brand, 16);
    gtk_widget_set_margin_top(brand, 14);
    gtk_widget_set_margin_bottom(brand, 12);
    GtkWidget *logo = gtk_drawing_area_new();
    gtk_widget_set_size_request(logo, 26, 26);
    g_signal_connect(logo, "draw", G_CALLBACK(logo_draw), NULL);
    gtk_box_pack_start(GTK_BOX(brand), logo, FALSE, FALSE, 0);
    GtkWidget *names = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *bn = gtk_label_new("DeepSeek Harness");
    gtk_label_set_xalign(GTK_LABEL(bn), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(bn), "brand");
    GtkWidget *ver = gtk_label_new("native · linux");
    gtk_label_set_xalign(GTK_LABEL(ver), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(ver), "brand-sub");
    gtk_box_pack_start(GTK_BOX(names), bn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(names), ver, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(brand), names, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), brand, FALSE, FALSE, 0);

    GtkWidget *nb = gtk_button_new();
    GtkWidget *nbb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(nbb, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(nbb), gtk_image_new_from_icon_name("document-edit-symbolic", GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(nbb), gtk_label_new("New Session"), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(nb), nbb);
    gtk_style_context_add_class(gtk_widget_get_style_context(nb), "new-session");
    gtk_widget_set_tooltip_text(nb, "New Session (Ctrl+N)");
    gtk_widget_set_margin_start(nb, 12);
    gtk_widget_set_margin_end(nb, 12);
    g_signal_connect(nb, "clicked", G_CALLBACK(new_clicked), s);
    gtk_box_pack_start(GTK_BOX(v), nb, FALSE, FALSE, 0);

    s->search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(s->search), "Search sessions");
    gtk_widget_set_margin_start(s->search, 12);
    gtk_widget_set_margin_end(s->search, 12);
    gtk_widget_set_margin_top(s->search, 10);
    g_signal_connect(s->search, "search-changed", G_CALLBACK(search_changed), s);
    gtk_box_pack_start(GTK_BOX(v), s->search, FALSE, FALSE, 0);

    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start(hdr, 16);
    gtk_widget_set_margin_end(hdr, 8);
    gtk_widget_set_margin_top(hdr, 12);
    gtk_widget_set_margin_bottom(hdr, 2);
    GtkWidget *hl = gtk_label_new("Workspaces");
    gtk_style_context_add_class(gtk_widget_get_style_context(hl), "dim-label");
    gtk_style_context_add_class(gtk_widget_get_style_context(hl), "section-title");
    gtk_label_set_xalign(GTK_LABEL(hl), 0);
    gtk_box_pack_start(GTK_BOX(hdr), hl, TRUE, TRUE, 0);
    GtkWidget *add = gtk_button_new_from_icon_name("folder-new-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(add), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(add, "Add workspace (Ctrl+O)");
    g_signal_connect(add, "clicked", G_CALLBACK(add_ws_clicked), s);
    gtk_box_pack_end(GTK_BOX(hdr), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), hdr, FALSE, FALSE, 0);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    s->list = gtk_list_box_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(s->list), "session-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(s->list), GTK_SELECTION_SINGLE);
    g_signal_connect(s->list, "row-activated", G_CALLBACK(row_activated), s);
    g_signal_connect(s->list, "row-selected", G_CALLBACK(row_selected), s);
    gtk_container_add(GTK_CONTAINER(sw), s->list);
    gtk_box_pack_start(GTK_BOX(v), sw, TRUE, TRUE, 0);

    GtkWidget *st = gtk_button_new();
    GtkWidget *stb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(stb), gtk_image_new_from_icon_name("emblem-system-symbolic", GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(stb), gtk_label_new("Settings"), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(st), stb);
    gtk_button_set_relief(GTK_BUTTON(st), GTK_RELIEF_NONE);
    gtk_widget_set_halign(st, GTK_ALIGN_START);
    gtk_widget_set_margin_start(st, 8);
    gtk_widget_set_margin_bottom(st, 8);
    gtk_widget_set_margin_top(st, 4);
    gtk_widget_set_tooltip_text(st, "Settings (Ctrl+,)");
    g_signal_connect(st, "clicked", G_CALLBACK(settings_clicked), s);
    gtk_box_pack_end(GTK_BOX(v), st, FALSE, FALSE, 0);

    store_observe(on_store, s);
    g_signal_connect(v, "destroy", G_CALLBACK(root_destroyed), s);
    s->clock_source = g_timeout_add_seconds(60, clock_tick, s);
    sidebar_reload(s);
    gtk_widget_show_all(v);
    return s;
}
