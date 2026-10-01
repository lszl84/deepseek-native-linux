#include "composer.h"
#include "theme.h"
#include "process.h"
#include "conversation.h"
#include <string.h>

ImageData *image_load_scaled(const char *path, int max_side, int *out_w, int *out_h);

const SlashCommand SLASH_COMMANDS[] = {
    { "/new", "Start a new session" },
    { "/stop", "Stop the running turn" },
    { "/retry", "Retry the last turn" },
    { "/rename", "Rename this session" },
    { "/copy", "Copy the last response" },
    { "/export", "Export the transcript as Markdown" },
    { "/settings", "Open settings" },
    { "/workspace", "Choose another workspace" },
};
const int N_SLASH = G_N_ELEMENTS(SLASH_COMMANDS);

struct Composer {
    ComposerCallbacks cb;
    GtkWidget *root;
    GtkWidget *text_view;
    GtkTextBuffer *buffer;
    GtkWidget *placeholder;
    GtkWidget *chips;
    GtkWidget *perm_button, *perm_icon, *perm_label;
    GtkWidget *model_button, *model_label, *effort_label;
    GtkWidget *perm_popover, *model_popover;
    GtkWidget *send;
    gboolean send_hover, send_pressed;
    GPtrArray *images; /* ImageData* */
    gboolean running;
    PermissionMode permission;
    char *model;
    Effort effort;
    /* completion */
    GtkWidget *comp_popover, *comp_list;
    GPtrArray *comp_values; /* char* */
    int comp_sel;
    int comp_start, comp_end; /* char offsets */
    guint comp_gen;
    gboolean dead;
    gboolean editing; /* programmatic buffer edit in progress */
};

static void update_send(Composer *c);
static void update_completion(Composer *c);
static void hide_completion(Composer *c);
static void rebuild_perm(Composer *c);
static void rebuild_model(Composer *c);

GtkWidget *composer_widget(Composer *c) { return c->root; }

static char *buffer_text(Composer *c) {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(c->buffer, &a, &b);
    return gtk_text_buffer_get_text(c->buffer, &a, &b, FALSE);
}

static gboolean is_empty(Composer *c) {
    char *t = buffer_text(c);
    gboolean e = str_blank(t) && c->images->len == 0;
    g_free(t);
    return e;
}

/* ---------------- send button ---------------- */

static gboolean send_draw(GtkWidget *w, cairo_t *cr, Composer *c) {
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double r = MIN(W, H) / 2 - 1;
    gboolean empty = is_empty(c);
    gboolean stop = c->running && empty;
    gboolean enabled = stop || !empty;
    RGBA col = th(C_ACCENT);
    if (!enabled) col = rgba_alpha(col, 0.35);
    else if (c->send_pressed) col = rgba_mix(col, (RGBA){ 0, 0, 0, 1 }, 0.2);
    else if (c->send_hover) col = rgba_mix(col, (RGBA){ 0, 0, 0, 1 }, 0.08);
    set_rgba(cr, col);
    cairo_new_path(cr);
    cairo_arc(cr, W / 2, H / 2, r, 0, 2 * G_PI);
    cairo_fill(cr);
    RGBA fg = th(C_ON_ACCENT);
    set_rgba(cr, fg);
    if (stop) {
        rounded_rect(cr, W / 2 - 5, H / 2 - 5, 10, 10, 2);
        cairo_fill(cr);
    } else {
        cairo_set_line_width(cr, 2);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_move_to(cr, W / 2, H / 2 + 6);
        cairo_line_to(cr, W / 2, H / 2 - 6);
        cairo_move_to(cr, W / 2 - 5, H / 2 - 1);
        cairo_line_to(cr, W / 2, H / 2 - 6);
        cairo_line_to(cr, W / 2 + 5, H / 2 - 1);
        cairo_stroke(cr);
    }
    return TRUE;
}

static void do_send(Composer *c);

static gboolean send_press(GtkWidget *w, GdkEventButton *ev, Composer *c) {
    if (ev->button != 1 || ev->type != GDK_BUTTON_PRESS) return FALSE;
    c->send_pressed = TRUE;
    gtk_widget_queue_draw(w);
    return TRUE;
}

static gboolean send_release(GtkWidget *w, GdkEventButton *ev, Composer *c) {
    if (ev->button != 1 || !c->send_pressed) return FALSE;
    c->send_pressed = FALSE;
    gtk_widget_queue_draw(w);
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    if (ev->x < 0 || ev->y < 0 || ev->x > W || ev->y > H) return TRUE;
    if (c->running && is_empty(c)) { if (c->cb.stop) c->cb.stop(c->cb.data); }
    else do_send(c);
    return TRUE;
}

static gboolean send_cross(GtkWidget *w, GdkEventCrossing *ev, Composer *c) {
    c->send_hover = ev->type == GDK_ENTER_NOTIFY;
    gtk_widget_queue_draw(w);
    return FALSE;
}

static void update_send(Composer *c) {
    gtk_widget_queue_draw(c->send);
    gtk_widget_set_tooltip_text(c->send, c->running && is_empty(c) ? "Stop (Ctrl+.)" : c->running ? "Send to the running agent" : "Send (Enter)");
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(c->buffer, &a, &b);
    gtk_widget_set_visible(c->placeholder, gtk_text_iter_equal(&a, &b));
}

/* ---------------- chips ---------------- */

static void rebuild_chips(Composer *c);

static void chip_remove(GtkButton *b, Composer *c) {
    guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "index"));
    if (i < c->images->len) g_ptr_array_remove_index(c->images, i);
    rebuild_chips(c);
    update_send(c);
}

static void rebuild_chips(Composer *c) {
    GList *ch = gtk_container_get_children(GTK_CONTAINER(c->chips));
    for (GList *l = ch; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(ch);
    for (guint i = 0; i < c->images->len; i++) {
        ImageData *d = g_ptr_array_index(c->images, i);
        GtkWidget *b = gtk_button_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_container_add(GTK_CONTAINER(box), gtk_image_new_from_icon_name("image-x-generic-symbolic", GTK_ICON_SIZE_MENU));
        GtkWidget *l = gtk_label_new(d->path ? path_base(d->path) : "image");
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 24);
        gtk_container_add(GTK_CONTAINER(box), l);
        gtk_container_add(GTK_CONTAINER(box), gtk_image_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU));
        gtk_container_add(GTK_CONTAINER(b), box);
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "chip");
        gtk_widget_set_tooltip_text(b, "Remove attachment");
        g_object_set_data(G_OBJECT(b), "index", GUINT_TO_POINTER(i));
        g_signal_connect(b, "clicked", G_CALLBACK(chip_remove), c);
        gtk_container_add(GTK_CONTAINER(c->chips), b);
    }
    gtk_widget_show_all(c->chips);
    gtk_widget_set_visible(c->chips, c->images->len > 0);
}

/* ---------------- attachments ---------------- */

static gboolean is_image_path(const char *p) {
    const char *exts[] = { ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".tif", ".tiff", ".heic", NULL };
    char *l = g_ascii_strdown(p, -1);
    gboolean r = FALSE;
    for (int i = 0; exts[i] && !r; i++) r = g_str_has_suffix(l, exts[i]);
    g_free(l);
    return r;
}

void composer_add_files(Composer *c, GSList *paths) {
    GString *mentions = g_string_new("");
    const char *cwd = c->cb.workspace ? c->cb.workspace(c->cb.data) : NULL;
    for (GSList *l = paths; l; l = l->next) {
        const char *p = l->data;
        if (is_image_path(p)) {
            ImageData *d = image_load_scaled(p, 1568, NULL, NULL);
            if (d) { g_ptr_array_add(c->images, d); continue; }
        }
        char *rel = cwd && g_str_has_prefix(p, cwd) && p[strlen(cwd)] == '/' ? g_strdup(p + strlen(cwd) + 1) : g_strdup(p);
        if (path_is_dir(p)) { char *t = g_strconcat(rel, "/", NULL); g_free(rel); rel = t; }
        if (mentions->len) g_string_append_c(mentions, ' ');
        if (strchr(rel, ' ')) g_string_append_printf(mentions, "@\"%s\"", rel);
        else g_string_append_printf(mentions, "@%s", rel);
        g_free(rel);
    }
    if (mentions->len) {
        char *cur = buffer_text(c);
        size_t n = strlen(cur);
        gboolean need_space = n && cur[n - 1] != ' ' && cur[n - 1] != '\n';
        g_free(cur);
        g_string_append_c(mentions, ' ');
        if (need_space) g_string_prepend_c(mentions, ' ');
        gtk_text_buffer_insert_at_cursor(c->buffer, mentions->str, -1);
    }
    g_string_free(mentions, TRUE);
    rebuild_chips(c);
    update_send(c);
    composer_focus(c);
}

static void attach_clicked(GtkButton *b, Composer *c) {
    GtkWidget *top = gtk_widget_get_toplevel(c->root);
    GtkFileChooserNative *fc = gtk_file_chooser_native_new("Attach Files", GTK_WINDOW(top), GTK_FILE_CHOOSER_ACTION_OPEN, "_Attach", "_Cancel");
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(fc), TRUE);
    const char *cwd = c->cb.workspace ? c->cb.workspace(c->cb.data) : NULL;
    if (cwd) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), cwd);
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(fc));
        composer_add_files(c, files);
        g_slist_free_full(files, g_free);
    }
    g_object_unref(fc);
}

static GSList *uris_to_paths(char **uris) {
    GSList *paths = NULL;
    for (int i = 0; uris && uris[i]; i++) {
        char *p = g_filename_from_uri(uris[i], NULL, NULL);
        if (p) paths = g_slist_append(paths, p);
    }
    return paths;
}

static void on_paste(GtkTextView *tv, Composer *c) {
    GtkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(tv), GDK_SELECTION_CLIPBOARD);
    if (gtk_clipboard_wait_is_uris_available(cb)) {
        char **uris = gtk_clipboard_wait_for_uris(cb);
        GSList *paths = uris_to_paths(uris);
        g_strfreev(uris);
        if (paths) {
            composer_add_files(c, paths);
            g_slist_free_full(paths, g_free);
            g_signal_stop_emission_by_name(tv, "paste-clipboard");
            return;
        }
    }
    if (!gtk_clipboard_wait_is_text_available(cb) && gtk_clipboard_wait_is_image_available(cb)) {
        GdkPixbuf *pb = gtk_clipboard_wait_for_image(cb);
        if (pb) {
            char *u = short_uuid();
            char *fn = g_strdup_printf("pasted-%s.png", u);
            char *path = g_build_filename(spill_dir(), fn, NULL);
            if (gdk_pixbuf_save(pb, path, "png", NULL, NULL)) {
                ImageData *d = image_load_scaled(path, 1568, NULL, NULL);
                if (d) g_ptr_array_add(c->images, d);
                rebuild_chips(c);
                update_send(c);
            }
            g_free(path);
            g_free(fn);
            g_free(u);
            g_object_unref(pb);
            g_signal_stop_emission_by_name(tv, "paste-clipboard");
        }
    }
}

#define URI_INFO 4242

static void on_drag_received(GtkWidget *w, GdkDragContext *ctx, int x, int y, GtkSelectionData *sel, guint info, guint time, Composer *c) {
    if (info != URI_INFO) return;
    char **uris = gtk_selection_data_get_uris(sel);
    GSList *paths = uris_to_paths(uris);
    g_strfreev(uris);
    if (paths) {
        composer_add_files(c, paths);
        g_slist_free_full(paths, g_free);
    }
    gtk_drag_finish(ctx, paths != NULL, FALSE, time);
    g_signal_stop_emission_by_name(w, "drag-data-received");
}

/* ---------------- sending ---------------- */

static void do_send(Composer *c) {
    hide_completion(c);
    char *raw = buffer_text(c);
    char *text = str_trim(raw);
    g_free(raw);
    if (!*text && !c->images->len) { g_free(text); return; }
    if (text[0] == '/' && !strchr(text, ' ') && !c->images->len) {
        for (int i = 0; i < N_SLASH; i++) {
            if (!strcmp(SLASH_COMMANDS[i].name, text)) {
                gtk_text_buffer_set_text(c->buffer, "", -1);
                if (c->cb.slash) c->cb.slash(text, c->cb.data);
                g_free(text);
                update_send(c);
                return;
            }
        }
    }
    GPtrArray *imgs = c->images;
    c->images = g_ptr_array_new_with_free_func(image_data_free);
    gtk_text_buffer_set_text(c->buffer, "", -1);
    rebuild_chips(c);
    if (c->cb.send) c->cb.send(text, imgs, c->cb.data);
    else g_ptr_array_unref(imgs);
    g_free(text);
    update_send(c);
}

/* ---------------- file index for @ completion ---------------- */

typedef struct {
    gint64 at;
    GPtrArray *files;
    gboolean loading;
} IndexEntry;

static GHashTable *file_index; /* cwd -> IndexEntry* */

typedef struct { char *cwd; Composer *c; } IndexJob;

static gint cmp_strp(gconstpointer a, gconstpointer b) { return strcmp(*(char **)a, *(char **)b); }

typedef struct { char *cwd; GPtrArray *files; Composer *c; } IndexDone;

static void index_done(gpointer p) {
    IndexDone *d = p;
    IndexEntry *e = g_hash_table_lookup(file_index, d->cwd);
    if (e) {
        if (e->files) g_ptr_array_unref(e->files);
        e->files = d->files;
        e->at = g_get_monotonic_time();
        e->loading = FALSE;
    } else g_ptr_array_unref(d->files);
    if (d->c && !d->c->dead && gtk_widget_is_focus(d->c->text_view)) update_completion(d->c);
    g_free(d->cwd);
    g_free(d);
}

static gpointer index_thread(gpointer p) {
    IndexJob *j = p;
    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    const char *rg = ripgrep_path();
    if (rg) {
        char *argv[] = { (char *)rg, "--files", "--hidden", "--no-messages", "-g", "!.git/", "-g", "!node_modules/", "-g", "!.build/", "-g", "!dist/",
                         "-g", "!build/", "-g", "!.venv/", "-g", "!__pycache__/", "-g", "!target/", NULL };
        int code;
        char *out = proc_run_capture(argv, j->cwd, 20, &code, NULL);
        char **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i] && files->len < 50000; i++)
            if (*lines[i]) g_ptr_array_add(files, g_strdup(lines[i]));
        g_strfreev(lines);
        g_free(out);
    }
    GHashTable *dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < files->len; i++) {
        char *p = g_path_get_dirname(g_ptr_array_index(files, i));
        while (strcmp(p, ".") && *p) {
            char *d = g_strconcat(p, "/", NULL);
            if (g_hash_table_contains(dirs, d)) { g_free(d); break; }
            g_hash_table_add(dirs, d);
            char *pp = g_path_get_dirname(p);
            g_free(p);
            p = pp;
        }
        g_free(p);
    }
    g_ptr_array_sort(files, cmp_strp);
    GPtrArray *all = g_ptr_array_new_with_free_func(g_free);
    GList *dl = g_hash_table_get_keys(dirs);
    dl = g_list_sort(dl, (GCompareFunc)strcmp);
    for (GList *l = dl; l; l = l->next) g_ptr_array_add(all, g_strdup(l->data));
    g_list_free(dl);
    g_hash_table_unref(dirs);
    for (guint i = 0; i < files->len; i++) g_ptr_array_add(all, g_strdup(g_ptr_array_index(files, i)));
    g_ptr_array_unref(files);
    IndexDone *d = g_new0(IndexDone, 1);
    d->cwd = j->cwd;
    d->files = all;
    d->c = j->c;
    main_async(index_done, d);
    g_free(j);
    return NULL;
}

static GPtrArray *index_files(Composer *c, const char *cwd) {
    if (!file_index) file_index = g_hash_table_new(g_str_hash, g_str_equal);
    IndexEntry *e = g_hash_table_lookup(file_index, cwd);
    if (!e) {
        e = g_new0(IndexEntry, 1);
        g_hash_table_insert(file_index, g_strdup(cwd), e);
    }
    gboolean stale = !e->files || g_get_monotonic_time() - e->at > 30 * G_USEC_PER_SEC;
    if (stale && !e->loading) {
        e->loading = TRUE;
        IndexJob *j = g_new0(IndexJob, 1);
        j->cwd = g_strdup(cwd);
        j->c = c;
        g_thread_unref(g_thread_new("file-index", index_thread, j));
    }
    return e->files;
}

typedef struct { int score; const char *f; } Scored;

static gint cmp_scored(gconstpointer a, gconstpointer b) { return ((Scored *)b)->score - ((Scored *)a)->score; }

static GPtrArray *fuzzy_match(const char *q, GPtrArray *files, int limit) {
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    if (!files) return out;
    if (!*q) {
        for (guint i = 0; i < files->len && (int)out->len < limit; i++) {
            const char *f = g_ptr_array_index(files, i);
            const char *sl = strchr(f, '/');
            if (!sl || (sl[1] == 0)) g_ptr_array_add(out, g_strdup(f));
        }
        return out;
    }
    char *ql = g_utf8_strdown(q, -1);
    size_t qn = strlen(ql);
    GArray *sc = g_array_new(FALSE, FALSE, sizeof(Scored));
    for (guint i = 0; i < files->len; i++) {
        const char *f = g_ptr_array_index(files, i);
        size_t fn = strlen(f);
        size_t end = fn && f[fn - 1] == '/' ? fn - 1 : fn;
        int base = 0;
        for (size_t k = 0; k < end; k++)
            if (f[k] == '/') base = k + 1;
        size_t qi = 0;
        int score = 0, last = -2;
        for (size_t k = 0; k < fn && qi < qn; k++) {
            if (g_ascii_tolower(f[k]) != ql[qi]) continue;
            score += ((int)k == last + 1) ? 6 : 1;
            if ((int)k >= base) score += 3;
            if ((int)k == base) score += 8;
            last = k;
            qi++;
        }
        if (qi == qn) {
            Scored s = { score * 100 - (int)fn, f };
            g_array_append_val(sc, s);
        }
    }
    g_array_sort(sc, cmp_scored);
    for (guint i = 0; i < sc->len && (int)i < limit; i++) g_ptr_array_add(out, g_strdup(g_array_index(sc, Scored, i).f));
    g_array_unref(sc);
    g_free(ql);
    return out;
}

/* ---------------- completion popup ---------------- */

static void hide_completion(Composer *c) {
    if (c->comp_popover && gtk_widget_get_visible(c->comp_popover)) gtk_widget_hide(c->comp_popover);
}

static gboolean completion_visible(Composer *c) { return c->comp_popover && gtk_widget_get_visible(c->comp_popover) && c->comp_values->len; }

static void select_row(Composer *c, int i) {
    if (!c->comp_values->len) return;
    c->comp_sel = CLAMP(i, 0, (int)c->comp_values->len - 1);
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(c->comp_list), c->comp_sel);
    if (row) {
        gtk_list_box_select_row(GTK_LIST_BOX(c->comp_list), row);
        GtkAdjustment *adj = gtk_list_box_get_adjustment(GTK_LIST_BOX(c->comp_list));
        if (adj) {
            GtkAllocation a;
            gtk_widget_get_allocation(GTK_WIDGET(row), &a);
            double v = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj);
            if (a.y < v) gtk_adjustment_set_value(adj, a.y);
            else if (a.y + a.height > v + page) gtk_adjustment_set_value(adj, a.y + a.height - page);
        }
    }
}

static void accept_completion(Composer *c) {
    if (c->comp_sel < 0 || c->comp_sel >= (int)c->comp_values->len) { hide_completion(c); return; }
    /* Editing the buffer re-runs completion and clears comp_values, so take what we need first. */
    char *v = g_strdup(g_ptr_array_index(c->comp_values, c->comp_sel));
    int start = c->comp_start, end = c->comp_end;
    hide_completion(c);
    c->editing = TRUE;
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(c->buffer, &a, start);
    gtk_text_buffer_get_iter_at_offset(c->buffer, &b, end);
    gtk_text_buffer_delete(c->buffer, &a, &b);
    if (v[0] == '/') {
        gtk_text_buffer_insert(c->buffer, &a, v, -1);
        c->editing = FALSE;
        g_free(v);
        do_send(c);
        return;
    }
    char *ins = strchr(v, ' ') ? g_strdup_printf("@\"%s\" ", v) : g_strdup_printf("@%s%s", v, g_str_has_suffix(v, "/") ? "" : " ");
    gtk_text_buffer_insert(c->buffer, &a, ins, -1);
    c->editing = FALSE;
    g_free(ins);
    g_free(v);
    update_send(c);
}

static void comp_row_activated(GtkListBox *lb, GtkListBoxRow *row, Composer *c) {
    c->comp_sel = gtk_list_box_row_get_index(row);
    accept_completion(c);
    composer_focus(c);
}

static void show_completion(Composer *c, GPtrArray *titles, GPtrArray *details) {
    if (!titles->len) { hide_completion(c); return; }
    if (!c->comp_popover) {
        c->comp_popover = gtk_popover_new(c->text_view);
        gtk_popover_set_modal(GTK_POPOVER(c->comp_popover), FALSE);
        gtk_popover_set_position(GTK_POPOVER(c->comp_popover), GTK_POS_TOP);
        gtk_style_context_add_class(gtk_widget_get_style_context(c->comp_popover), "completion");
        GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), 320);
        gtk_widget_set_size_request(sw, 420, -1);
        c->comp_list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(c->comp_list), GTK_SELECTION_SINGLE);
        gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(c->comp_list), TRUE);
        g_signal_connect(c->comp_list, "row-activated", G_CALLBACK(comp_row_activated), c);
        gtk_widget_set_can_focus(c->comp_list, FALSE);
        gtk_container_add(GTK_CONTAINER(sw), c->comp_list);
        gtk_container_add(GTK_CONTAINER(c->comp_popover), sw);
    }
    GList *ch = gtk_container_get_children(GTK_CONTAINER(c->comp_list));
    for (GList *l = ch; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(ch);
    for (guint i = 0; i < titles->len; i++) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(box, 10);
        gtk_widget_set_margin_end(box, 10);
        gtk_widget_set_margin_top(box, 4);
        gtk_widget_set_margin_bottom(box, 4);
        GtkWidget *t = gtk_label_new(g_ptr_array_index(titles, i));
        gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_xalign(GTK_LABEL(t), 0);
        gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
        const char *d = details ? g_ptr_array_index(details, i) : NULL;
        if (d && *d) {
            GtkWidget *dl = gtk_label_new(d);
            gtk_style_context_add_class(gtk_widget_get_style_context(dl), "dim-label");
            gtk_label_set_ellipsize(GTK_LABEL(dl), PANGO_ELLIPSIZE_END);
            gtk_label_set_xalign(GTK_LABEL(dl), 0);
            gtk_box_pack_start(GTK_BOX(box), dl, TRUE, TRUE, 0);
        }
        GtkWidget *row = gtk_list_box_row_new();
        gtk_widget_set_can_focus(row, FALSE);
        gtk_container_add(GTK_CONTAINER(row), box);
        gtk_container_add(GTK_CONTAINER(c->comp_list), row);
    }
    gtk_widget_show_all(c->comp_list);
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(c->buffer, &it, c->comp_start);
    GdkRectangle r;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(c->text_view), &it, &r);
    int wx, wy;
    gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(c->text_view), GTK_TEXT_WINDOW_WIDGET, r.x, r.y, &wx, &wy);
    GdkRectangle pt = { wx, wy, 1, r.height };
    gtk_popover_set_pointing_to(GTK_POPOVER(c->comp_popover), &pt);
    gtk_widget_show_all(c->comp_popover);
    select_row(c, 0);
}

static void update_completion(Composer *c) {
    GtkTextIter ins, start;
    gtk_text_buffer_get_iter_at_mark(c->buffer, &ins, gtk_text_buffer_get_insert(c->buffer));
    if (gtk_text_buffer_get_has_selection(c->buffer)) { hide_completion(c); return; }
    start = ins;
    while (!gtk_text_iter_is_start(&start)) {
        GtkTextIter prev = start;
        gtk_text_iter_backward_char(&prev);
        gunichar ch = gtk_text_iter_get_char(&prev);
        if (ch == ' ' || ch == '\n' || ch == '\t') break;
        start = prev;
    }
    char *token = gtk_text_buffer_get_text(c->buffer, &start, &ins, FALSE);
    c->comp_start = gtk_text_iter_get_offset(&start);
    c->comp_end = gtk_text_iter_get_offset(&ins);
    g_ptr_array_set_size(c->comp_values, 0);
    if (token[0] == '@') {
        const char *cwd = c->cb.workspace ? c->cb.workspace(c->cb.data) : NULL;
        if (!cwd) { hide_completion(c); g_free(token); return; }
        GPtrArray *files = index_files(c, cwd);
        const char *q = token + 1;
        if (*q == '"') q++;
        GPtrArray *m = fuzzy_match(q, files, 12);
        for (guint i = 0; i < m->len; i++) g_ptr_array_add(c->comp_values, g_strdup(g_ptr_array_index(m, i)));
        show_completion(c, m, NULL);
        g_ptr_array_unref(m);
    } else if (token[0] == '/' && c->comp_start == 0) {
        GPtrArray *titles = g_ptr_array_new(), *details = g_ptr_array_new();
        for (int i = 0; i < N_SLASH; i++) {
            if (!g_str_has_prefix(SLASH_COMMANDS[i].name, token)) continue;
            g_ptr_array_add(titles, (gpointer)SLASH_COMMANDS[i].name);
            g_ptr_array_add(details, (gpointer)SLASH_COMMANDS[i].detail);
            g_ptr_array_add(c->comp_values, g_strdup(SLASH_COMMANDS[i].name));
        }
        show_completion(c, titles, details);
        g_ptr_array_unref(titles);
        g_ptr_array_unref(details);
    } else hide_completion(c);
    g_free(token);
}

/* ---------------- text view events ---------------- */

static gboolean tv_key(GtkWidget *w, GdkEventKey *ev, Composer *c) {
    guint k = ev->keyval;
    gboolean shift = ev->state & GDK_SHIFT_MASK, ctrl = ev->state & GDK_CONTROL_MASK, alt = ev->state & GDK_MOD1_MASK;
    if (completion_visible(c)) {
        switch (k) {
        case GDK_KEY_Down: select_row(c, c->comp_sel + 1); return TRUE;
        case GDK_KEY_Up: select_row(c, c->comp_sel - 1); return TRUE;
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_Tab: accept_completion(c); return TRUE;
        case GDK_KEY_Escape: hide_completion(c); return TRUE;
        default: break;
        }
    }
    if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter) {
        if (gtk_text_view_im_context_filter_keypress(GTK_TEXT_VIEW(w), ev)) return TRUE;
        gboolean send = ctrl || (!shift && !alt && settings()->send_on_enter);
        if (send) { do_send(c); return TRUE; }
        if (shift || alt) {
            gtk_text_buffer_insert_at_cursor(c->buffer, "\n", 1);
            return TRUE;
        }
    }
    return FALSE;
}

static void buffer_changed(GtkTextBuffer *b, Composer *c) {
    update_send(c);
    if (!c->editing) update_completion(c);
}

static void mark_set(GtkTextBuffer *b, GtkTextIter *it, GtkTextMark *m, Composer *c) {
    if (!c->editing && m == gtk_text_buffer_get_insert(b) && c->comp_popover && gtk_widget_get_visible(c->comp_popover)) update_completion(c);
}

static gboolean tv_focus_out(GtkWidget *w, GdkEventFocus *ev, Composer *c) {
    hide_completion(c);
    return FALSE;
}

/* ---------------- popover menus ---------------- */

static GtkWidget *menu_row(const char *title, const char *detail, gboolean checked, const char *icon) {
    GtkWidget *b = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "menu-row");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *check = gtk_image_new_from_icon_name("object-select-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_opacity(check, checked ? 1 : 0);
    gtk_box_pack_start(GTK_BOX(box), check, FALSE, FALSE, 0);
    if (icon) gtk_box_pack_start(GTK_BOX(box), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget *t = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    if (detail) {
        GtkWidget *d = gtk_label_new(detail);
        gtk_label_set_xalign(GTK_LABEL(d), 0);
        gtk_label_set_line_wrap(GTK_LABEL(d), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(d), 40);
        gtk_style_context_add_class(gtk_widget_get_style_context(d), "dim-label");
        gtk_style_context_add_class(gtk_widget_get_style_context(d), "caption");
        gtk_box_pack_start(GTK_BOX(v), d, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), v, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(b), box);
    return b;
}

static GtkWidget *menu_header(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_margin_start(l, 12);
    gtk_widget_set_margin_top(l, 6);
    gtk_widget_set_margin_bottom(l, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "caption-heading");
    return l;
}

static void perm_pick(GtkButton *b, Composer *c) {
    PermissionMode m = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "mode"));
    gtk_popover_popdown(GTK_POPOVER(c->perm_popover));
    composer_set_permission(c, m);
    if (c->cb.permission_changed) c->cb.permission_changed(m, c->cb.data);
}

static void rebuild_perm(Composer *c) {
    GtkWidget *box = gtk_bin_get_child(GTK_BIN(c->perm_popover));
    if (box) gtk_widget_destroy(box);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_box_pack_start(GTK_BOX(box), menu_header("Permissions"), FALSE, FALSE, 0);
    for (int m = PERM_READ_ONLY; m <= PERM_FULL_ACCESS; m++) {
        GtkWidget *r = menu_row(perm_label(m), perm_detail(m), m == (int)c->permission, perm_icon(m));
        g_object_set_data(G_OBJECT(r), "mode", GINT_TO_POINTER(m));
        g_signal_connect(r, "clicked", G_CALLBACK(perm_pick), c);
        gtk_box_pack_start(GTK_BOX(box), r, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(box);
    gtk_container_add(GTK_CONTAINER(c->perm_popover), box);
    gtk_image_set_from_icon_name(GTK_IMAGE(c->perm_icon), perm_icon(c->permission), GTK_ICON_SIZE_MENU);
    gtk_label_set_text(GTK_LABEL(c->perm_label), perm_label(c->permission));
    GtkStyleContext *sc = gtk_widget_get_style_context(c->perm_button);
    if (c->permission == PERM_FULL_ACCESS) gtk_style_context_add_class(sc, "warning");
    else gtk_style_context_remove_class(sc, "warning");
}

static void model_pick(GtkButton *b, Composer *c) {
    const char *id = g_object_get_data(G_OBJECT(b), "model");
    gtk_popover_popdown(GTK_POPOVER(c->model_popover));
    composer_set_model(c, id, c->effort);
    if (c->cb.model_changed) c->cb.model_changed(c->model, c->effort, c->cb.data);
}

static void effort_pick(GtkButton *b, Composer *c) {
    Effort e = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "effort"));
    gtk_popover_popdown(GTK_POPOVER(c->model_popover));
    composer_set_model(c, c->model, e);
    if (c->cb.model_changed) c->cb.model_changed(c->model, c->effort, c->cb.data);
}

static void rebuild_model(Composer *c) {
    GtkWidget *box = gtk_bin_get_child(GTK_BIN(c->model_popover));
    if (box) gtk_widget_destroy(box);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_box_pack_start(GTK_BOX(box), menu_header("Model"), FALSE, FALSE, 0);
    for (int i = 0; i < N_MODELS; i++) {
        GtkWidget *r = menu_row(MODELS[i].name, MODELS[i].detail, !g_strcmp0(MODELS[i].id, c->model), NULL);
        g_object_set_data(G_OBJECT(r), "model", (gpointer)MODELS[i].id);
        g_signal_connect(r, "clicked", G_CALLBACK(model_pick), c);
        gtk_box_pack_start(GTK_BOX(box), r, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), menu_header("Reasoning effort"), FALSE, FALSE, 0);
    for (int e = EFFORT_OFF; e <= EFFORT_MAX; e++) {
        GtkWidget *r = menu_row(effort_label(e), NULL, e == (int)c->effort, NULL);
        g_object_set_data(G_OBJECT(r), "effort", GINT_TO_POINTER(e));
        g_signal_connect(r, "clicked", G_CALLBACK(effort_pick), c);
        gtk_box_pack_start(GTK_BOX(box), r, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(box);
    gtk_container_add(GTK_CONTAINER(c->model_popover), box);
    gtk_label_set_text(GTK_LABEL(c->model_label), model_name(c->model));
    gtk_label_set_text(GTK_LABEL(c->effort_label), effort_label(c->effort));
}

static GtkWidget *pill_button(GtkWidget **icon, GtkWidget **label, GtkWidget **label2, GtkWidget **popover) {
    GtkWidget *b = gtk_menu_button_new();
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "pill");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    if (icon) { *icon = gtk_image_new(); gtk_box_pack_start(GTK_BOX(box), *icon, FALSE, FALSE, 0); }
    *label = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(box), *label, FALSE, FALSE, 0);
    if (label2) {
        *label2 = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(*label2), "dim-label");
        gtk_box_pack_start(GTK_BOX(box), *label2, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), gtk_image_new_from_icon_name("pan-down-symbolic", GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(b), box);
    *popover = gtk_popover_new(b);
    gtk_style_context_add_class(gtk_widget_get_style_context(*popover), "menu-popover");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(b), *popover);
    gtk_menu_button_set_direction(GTK_MENU_BUTTON(b), GTK_ARROW_UP);
    return b;
}

static gboolean root_click(GtkWidget *w, GdkEventButton *ev, Composer *c) {
    composer_focus(c);
    return FALSE;
}

/* ---------------- construction ---------------- */

static void composer_mark_dead(Composer *c) { c->dead = TRUE; }

Composer *composer_new(ComposerCallbacks cb) {
    Composer *c = g_new0(Composer, 1);
    c->cb = cb;
    c->images = g_ptr_array_new_with_free_func(image_data_free);
    c->comp_values = g_ptr_array_new_with_free_func(g_free);
    c->model = g_strdup(settings()->model);
    c->effort = settings()->effort;
    c->permission = settings()->permission;

    GtkWidget *ev = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(ev), FALSE);
    g_signal_connect(ev, "button-press-event", G_CALLBACK(root_click), c);
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame), "composer");
    gtk_container_add(GTK_CONTAINER(ev), frame);
    c->root = ev;

    c->chips = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(c->chips, 14);
    gtk_widget_set_margin_end(c->chips, 14);
    gtk_widget_set_margin_top(c->chips, 10);
    gtk_widget_set_no_show_all(c->chips, TRUE);
    gtk_box_pack_start(GTK_BOX(frame), c->chips, FALSE, FALSE, 0);

    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 24);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), 220);
    c->text_view = gtk_text_view_new();
    c->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(c->text_view));
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(c->text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(c->text_view), FALSE);
    gtk_text_view_set_pixels_inside_wrap(GTK_TEXT_VIEW(c->text_view), 3);
    gtk_text_view_set_pixels_below_lines(GTK_TEXT_VIEW(c->text_view), 3);
    gtk_style_context_add_class(gtk_widget_get_style_context(c->text_view), "composer-text");
    gtk_container_add(GTK_CONTAINER(sw), c->text_view);
    gtk_container_add(GTK_CONTAINER(overlay), sw);
    c->placeholder = gtk_label_new("Message or run a task, / commands, @ files");
    gtk_label_set_xalign(GTK_LABEL(c->placeholder), 0);
    gtk_label_set_ellipsize(GTK_LABEL(c->placeholder), PANGO_ELLIPSIZE_END);
    gtk_widget_set_valign(c->placeholder, GTK_ALIGN_START);
    gtk_widget_set_halign(c->placeholder, GTK_ALIGN_FILL);
    gtk_style_context_add_class(gtk_widget_get_style_context(c->placeholder), "placeholder");
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), c->placeholder);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), c->placeholder, TRUE);
    gtk_widget_set_margin_start(overlay, 16);
    gtk_widget_set_margin_end(overlay, 16);
    gtk_widget_set_margin_top(overlay, 12);
    gtk_box_pack_start(GTK_BOX(frame), overlay, TRUE, TRUE, 0);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_bottom(bar, 8);
    GtkWidget *attach = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(attach), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(attach, "Attach files or images");
    gtk_style_context_add_class(gtk_widget_get_style_context(attach), "circular");
    g_signal_connect(attach, "clicked", G_CALLBACK(attach_clicked), c);
    gtk_box_pack_start(GTK_BOX(bar), attach, FALSE, FALSE, 0);
    c->perm_button = pill_button(&c->perm_icon, &c->perm_label, NULL, &c->perm_popover);
    gtk_widget_set_tooltip_text(c->perm_button, "Permission mode");
    gtk_box_pack_start(GTK_BOX(bar), c->perm_button, FALSE, FALSE, 0);
    c->send = gtk_drawing_area_new();
    gtk_widget_set_size_request(c->send, 34, 34);
    gtk_widget_set_valign(c->send, GTK_ALIGN_CENTER);
    gtk_widget_add_events(c->send, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(c->send, "draw", G_CALLBACK(send_draw), c);
    g_signal_connect(c->send, "button-press-event", G_CALLBACK(send_press), c);
    g_signal_connect(c->send, "button-release-event", G_CALLBACK(send_release), c);
    g_signal_connect(c->send, "enter-notify-event", G_CALLBACK(send_cross), c);
    g_signal_connect(c->send, "leave-notify-event", G_CALLBACK(send_cross), c);
    gtk_box_pack_end(GTK_BOX(bar), c->send, FALSE, FALSE, 0);
    c->model_button = pill_button(NULL, &c->model_label, &c->effort_label, &c->model_popover);
    gtk_widget_set_tooltip_text(c->model_button, "Model and reasoning effort");
    gtk_widget_set_margin_end(c->model_button, 6);
    gtk_box_pack_end(GTK_BOX(bar), c->model_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(frame), bar, FALSE, FALSE, 0);

    g_signal_connect(c->text_view, "key-press-event", G_CALLBACK(tv_key), c);
    g_signal_connect(c->text_view, "paste-clipboard", G_CALLBACK(on_paste), c);
    g_signal_connect(c->text_view, "focus-out-event", G_CALLBACK(tv_focus_out), c);
    g_signal_connect(c->buffer, "changed", G_CALLBACK(buffer_changed), c);
    g_signal_connect(c->buffer, "mark-set", G_CALLBACK(mark_set), c);
    GtkTargetList *tl = gtk_drag_dest_get_target_list(c->text_view);
    if (tl) gtk_target_list_add_uri_targets(tl, URI_INFO);
    g_signal_connect(c->text_view, "drag-data-received", G_CALLBACK(on_drag_received), c);

    rebuild_perm(c);
    rebuild_model(c);
    g_signal_connect_swapped(c->root, "destroy", G_CALLBACK(composer_mark_dead), c);
    gtk_widget_show_all(c->root);
    update_send(c);
    return c;
}

void composer_set_running(Composer *c, gboolean running) {
    if (c->running == running) return;
    c->running = running;
    update_send(c);
}

void composer_set_placeholder(Composer *c, const char *text) {
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(c->placeholder)), text)) gtk_label_set_text(GTK_LABEL(c->placeholder), text);
}

void composer_set_permission(Composer *c, PermissionMode p) {
    c->permission = p;
    rebuild_perm(c);
}

void composer_set_model(Composer *c, const char *model, Effort effort) {
    if (model != c->model) {
        char *m = g_strdup(model);
        g_free(c->model);
        c->model = m;
    }
    c->effort = effort;
    rebuild_model(c);
}

PermissionMode composer_permission(Composer *c) { return c->permission; }
const char *composer_model(Composer *c) { return c->model; }
Effort composer_effort(Composer *c) { return c->effort; }

void composer_focus(Composer *c) { gtk_widget_grab_focus(c->text_view); }

void composer_set_text(Composer *c, const char *text) {
    gtk_text_buffer_set_text(c->buffer, text, -1);
    update_send(c);
}

void composer_insert_text(Composer *c, const char *text) { gtk_text_buffer_insert_at_cursor(c->buffer, text, -1); }

void composer_forward_key(Composer *c, GdkEventKey *ev) {
    composer_focus(c);
    gunichar u = gdk_keyval_to_unicode(ev->keyval);
    if (u) {
        char buf[8];
        int n = g_unichar_to_utf8(u, buf);
        gtk_text_buffer_insert_at_cursor(c->buffer, buf, n);
    }
}

#ifdef DSN_TEST_HOOKS
void composer_debug_popovers(Composer *c, GtkWidget **out, int *n) {
    *n = 0;
    GtkWidget *all[] = { c->perm_popover, c->model_popover, c->comp_popover };
    for (int i = 0; i < 3; i++)
        if (all[i] && gtk_widget_get_visible(all[i])) out[(*n)++] = all[i];
}
#endif
