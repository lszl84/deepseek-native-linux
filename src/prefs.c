#include "prefs.h"
#include "settings.h"
#include <string.h>

static GtkWidget *win;
static void (*changed_cb)(gpointer);
static gpointer changed_data;
static GtkWidget *key_entry, *key_status;

static void notify_changed(void) {
    settings_save();
    if (changed_cb) changed_cb(changed_data);
}

static GtkWidget *section(const char *title) {
    GtkWidget *l = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "prefs-section");
    gtk_widget_set_margin_top(l, 8);
    return l;
}

static GtkWidget *row(GtkWidget *grid, int r, const char *label, const char *hint, GtkWidget *control) {
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(v), l, FALSE, FALSE, 0);
    if (hint) {
        GtkWidget *h = gtk_label_new(hint);
        gtk_label_set_xalign(GTK_LABEL(h), 0);
        gtk_label_set_line_wrap(GTK_LABEL(h), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(h), 46);
        gtk_style_context_add_class(gtk_widget_get_style_context(h), "dim-label");
        gtk_style_context_add_class(gtk_widget_get_style_context(h), "caption");
        gtk_box_pack_start(GTK_BOX(v), h, FALSE, FALSE, 0);
    }
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(GTK_GRID(grid), v, 0, r, 1, 1);
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(control, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), control, 1, r, 1, 1);
    return control;
}

static GtkWidget *page_grid(void) {
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 14);
    gtk_grid_set_column_spacing(GTK_GRID(g), 24);
    gtk_widget_set_margin_start(g, 24);
    gtk_widget_set_margin_end(g, 24);
    gtk_widget_set_margin_top(g, 18);
    gtk_widget_set_margin_bottom(g, 18);
    return g;
}

/* ---- General ---- */

static void appearance_changed(GtkComboBox *c, gpointer p) {
    settings()->appearance = gtk_combo_box_get_active(c);
    notify_changed();
}

static void send_enter_changed(GtkSwitch *s, GParamSpec *ps, gpointer p) {
    settings()->send_on_enter = gtk_switch_get_active(s);
    notify_changed();
}

/* ---- Models ---- */

static void update_key_status(void) {
    gboolean present = api_key_present();
    char *t = present ? g_strdup_printf("Using a key from %s.", api_key_source())
                      : g_strdup("No API key found. Paste one here and press Save, or set DEEPSEEK_API_KEY.");
    gtk_label_set_text(GTK_LABEL(key_status), t);
    g_free(t);
}

static void key_save(GtkButton *b, gpointer p) {
    const char *k = gtk_entry_get_text(GTK_ENTRY(key_entry));
    if (!*k) return;
    api_key_store(k);
    gtk_entry_set_text(GTK_ENTRY(key_entry), "");
    update_key_status();
    notify_changed();
}

static void key_clear(GtkButton *b, gpointer p) {
    api_key_store("");
    update_key_status();
    notify_changed();
}

static void base_changed(GtkEntry *e, gpointer p) {
    settings_set_base_url(gtk_entry_get_text(e));
    notify_changed();
}

static void model_changed(GtkComboBox *c, gpointer p) {
    const char *id = gtk_combo_box_get_active_id(c);
    if (!id) return;
    settings_set_model(id);
    notify_changed();
}

static void effort_changed(GtkComboBox *c, gpointer p) {
    settings()->effort = gtk_combo_box_get_active(c);
    notify_changed();
}

static void ctx_changed(GtkSpinButton *s, gpointer p) {
    settings()->context_window = (gint64)gtk_spin_button_get_value(s) * 1000;
    notify_changed();
}

static void maxtok_changed(GtkSpinButton *s, gpointer p) {
    settings()->max_tokens = (int)gtk_spin_button_get_value(s) * 1000;
    notify_changed();
}

/* ---- Agent ---- */

static void perm_changed(GtkComboBox *c, gpointer p) {
    settings()->permission = gtk_combo_box_get_active(c);
    notify_changed();
}

static void timeout_changed(GtkSpinButton *s, gpointer p) {
    settings()->bash_timeout_ms = (int)gtk_spin_button_get_value(s) * 1000;
    notify_changed();
}

static void web_changed(GtkSwitch *s, GParamSpec *ps, gpointer p) {
    settings()->web_search = gtk_switch_get_active(s);
    notify_changed();
}

static void sub_changed(GtkSwitch *s, GParamSpec *ps, gpointer p) {
    settings()->subagents = gtk_switch_get_active(s);
    notify_changed();
}

static void custom_changed(GtkTextBuffer *b, gpointer p) {
    GtkTextIter a, e;
    gtk_text_buffer_get_bounds(b, &a, &e);
    char *t = gtk_text_buffer_get_text(b, &a, &e, FALSE);
    settings_set_custom(t);
    g_free(t);
    notify_changed();
}

static GtkWidget *build(void) {
    Settings *s = settings();
    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    /* General */
    GtkWidget *g = page_grid();
    int r = 0;
    gtk_grid_attach(GTK_GRID(g), section("Appearance"), 0, r++, 2, 1);
    GtkWidget *ap = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ap), "Follow system");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ap), "Light");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ap), "Dark");
    gtk_combo_box_set_active(GTK_COMBO_BOX(ap), s->appearance);
    g_signal_connect(ap, "changed", G_CALLBACK(appearance_changed), NULL);
    row(g, r++, "Theme", "Follows the GTK theme and the desktop color scheme by default.", ap);
    gtk_grid_attach(GTK_GRID(g), section("Message box"), 0, r++, 2, 1);
    GtkWidget *se = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(se), s->send_on_enter);
    g_signal_connect(se, "notify::active", G_CALLBACK(send_enter_changed), NULL);
    row(g, r++, "Send with Enter", "When off, Enter adds a line and Ctrl+Enter sends.", se);
    gtk_stack_add_titled(GTK_STACK(stack), g, "general", "General");

    /* Models */
    g = page_grid();
    r = 0;
    gtk_grid_attach(GTK_GRID(g), section("DeepSeek API"), 0, r++, 2, 1);
    GtkWidget *kb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    key_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(key_entry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(key_entry), "sk-…");
    gtk_entry_set_width_chars(GTK_ENTRY(key_entry), 26);
    gtk_entry_set_input_purpose(GTK_ENTRY(key_entry), GTK_INPUT_PURPOSE_PASSWORD);
    g_signal_connect(key_entry, "activate", G_CALLBACK(key_save), NULL);
    GtkWidget *save = gtk_button_new_with_label("Save");
    g_signal_connect(save, "clicked", G_CALLBACK(key_save), NULL);
    GtkWidget *clear = gtk_button_new_with_label("Forget");
    g_signal_connect(clear, "clicked", G_CALLBACK(key_clear), NULL);
    gtk_box_pack_start(GTK_BOX(kb), key_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(kb), save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(kb), clear, FALSE, FALSE, 0);
    row(g, r++, "API key", "Stored in the system keyring, never in a file.", kb);
    key_status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(key_status), 0);
    gtk_label_set_line_wrap(GTK_LABEL(key_status), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(key_status), "dim-label");
    gtk_style_context_add_class(gtk_widget_get_style_context(key_status), "caption");
    gtk_grid_attach(GTK_GRID(g), key_status, 0, r++, 2, 1);
    update_key_status();
    GtkWidget *base = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(base), s->base_url);
    gtk_entry_set_width_chars(GTK_ENTRY(base), 34);
    g_signal_connect(base, "changed", G_CALLBACK(base_changed), NULL);
    row(g, r++, "Base URL", "Anthropic-compatible Messages endpoint.", base);
    gtk_grid_attach(GTK_GRID(g), section("Defaults for new sessions"), 0, r++, 2, 1);
    GtkWidget *mc = gtk_combo_box_text_new();
    for (int i = 0; i < N_MODELS; i++) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mc), MODELS[i].id, MODELS[i].name);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(mc), s->model);
    g_signal_connect(mc, "changed", G_CALLBACK(model_changed), NULL);
    row(g, r++, "Model", NULL, mc);
    GtkWidget *ec = gtk_combo_box_text_new();
    for (int e = EFFORT_OFF; e <= EFFORT_MAX; e++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ec), effort_label(e));
    gtk_combo_box_set_active(GTK_COMBO_BOX(ec), s->effort);
    g_signal_connect(ec, "changed", G_CALLBACK(effort_changed), NULL);
    row(g, r++, "Reasoning effort", NULL, ec);
    GtkWidget *cw = gtk_spin_button_new_with_range(16, 2000, 16);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(cw), s->context_window / 1000.0);
    g_signal_connect(cw, "value-changed", G_CALLBACK(ctx_changed), NULL);
    row(g, r++, "Context window (K tokens)", "Older history is summarized once the prompt passes 80% of this.", cw);
    GtkWidget *mt = gtk_spin_button_new_with_range(4, 512, 4);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(mt), s->max_tokens / 1000.0);
    g_signal_connect(mt, "value-changed", G_CALLBACK(maxtok_changed), NULL);
    row(g, r++, "Max output (K tokens)", NULL, mt);
    gtk_stack_add_titled(GTK_STACK(stack), g, "models", "Models");

    /* Agent */
    g = page_grid();
    r = 0;
    gtk_grid_attach(GTK_GRID(g), section("Permissions"), 0, r++, 2, 1);
    GtkWidget *pc = gtk_combo_box_text_new();
    for (int p = PERM_READ_ONLY; p <= PERM_FULL_ACCESS; p++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pc), perm_label(p));
    gtk_combo_box_set_active(GTK_COMBO_BOX(pc), s->permission);
    g_signal_connect(pc, "changed", G_CALLBACK(perm_changed), NULL);
    row(g, r++, "Default mode", "Shell commands run in a Landlock sandbox in Read Only and Workspace Write.", pc);
    GtkWidget *to = gtk_spin_button_new_with_range(5, 600, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(to), s->bash_timeout_ms / 1000.0);
    g_signal_connect(to, "value-changed", G_CALLBACK(timeout_changed), NULL);
    row(g, r++, "Command timeout (seconds)", "Commands still running after this move to a background job.", to);
    gtk_grid_attach(GTK_GRID(g), section("Tools"), 0, r++, 2, 1);
    GtkWidget *ws = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(ws), s->web_search);
    g_signal_connect(ws, "notify::active", G_CALLBACK(web_changed), NULL);
    row(g, r++, "Web search", "DeepSeek native search. Applies to new sessions.", ws);
    GtkWidget *sa = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sa), s->subagents);
    g_signal_connect(sa, "notify::active", G_CALLBACK(sub_changed), NULL);
    row(g, r++, "Subagents", "Let the agent delegate independent work. Applies to new sessions.", sa);
    gtk_grid_attach(GTK_GRID(g), section("Custom instructions"), 0, r++, 2, 1);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_widget_set_size_request(sw, -1, 120);
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tv), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 6);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_text_buffer_set_text(buf, s->custom_instructions, -1);
    g_signal_connect(buf, "changed", G_CALLBACK(custom_changed), NULL);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_widget_set_hexpand(sw, TRUE);
    gtk_grid_attach(GTK_GRID(g), sw, 0, r++, 2, 1);
    GtkWidget *hint = gtk_label_new("Added to the system prompt of every session, alongside AGENTS.md or CLAUDE.md from the workspace.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "caption");
    gtk_grid_attach(GTK_GRID(g), hint, 0, r++, 2, 1);
    gtk_stack_add_titled(GTK_STACK(stack), g, "agent", "Agent");
    return stack;
}

static void on_destroy(GtkWidget *w, gpointer p) { win = NULL; }

GtkWidget *prefs_window(void) { return win; }

void prefs_show(GtkWindow *parent, void (*on_change)(gpointer), gpointer data) {
    changed_cb = on_change;
    changed_data = data;
    if (win) { gtk_window_present(GTK_WINDOW(win)); return; }
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_title(GTK_WINDOW(win), "Settings");
    gtk_window_set_default_size(GTK_WINDOW(win), 620, 560);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DIALOG);
    GtkWidget *hb = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(hb), TRUE);
    GtkWidget *stack = build();
    GtkWidget *sw = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(sw), GTK_STACK(stack));
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(hb), sw);
    gtk_window_set_titlebar(GTK_WINDOW(win), hb);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), stack);
    gtk_container_add(GTK_CONTAINER(win), scroll);
    g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), NULL);
    gtk_widget_show_all(win);
    const char *page = g_getenv("DSN_SETTINGS_PAGE");
    if (page) gtk_stack_set_visible_child_name(GTK_STACK(stack), page);
}
