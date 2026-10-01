#include "theme.h"
#include "util.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

static RGBA palette[N_COLORS];
static gboolean dark;
static guint generation = 1;
static PangoFontDescription *fonts[N_FONTS];
static char *ui_family;
static double base_pt = 11;
static char *mono_family;
static PangoContext *pango_ctx;
static GHashTable *icon_cache;
static GHashTable *width_cache;
static int ui_scale = 1;
static int forced_appearance; /* 0 follow theme, 1 light, 2 dark */

void theme_force_appearance(int mode) { forced_appearance = mode; }

static RGBA rgb(int r, int g, int b, double a) { return (RGBA){ r / 255.0, g / 255.0, b / 255.0, a }; }
static RGBA dyn(gboolean d, RGBA l, RGBA k) { return d ? k : l; }

RGBA rgba_alpha(RGBA c, double a) { c.a *= a; return c; }

RGBA rgba_mix(RGBA a, RGBA b, double t) {
    return (RGBA){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}

static double luminance(RGBA c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }

static gboolean lookup(GtkStyleContext *sc, const char *name, RGBA *out) {
    GdkRGBA c;
    if (!gtk_style_context_lookup_color(sc, name, &c)) return FALSE;
    *out = (RGBA){ c.red, c.green, c.blue, c.alpha };
    return TRUE;
}

static void build_palette(RGBA bg, RGBA fg, RGBA accent, RGBA base, RGBA sidebar) {
    gboolean d = dark;
    palette[C_BG] = bg;
    palette[C_SIDEBAR_BG] = sidebar;
    palette[C_TEXT] = fg;
    palette[C_TEXT2] = rgba_mix(fg, bg, d ? 0.20 : 0.30);
    palette[C_TEXT3] = rgba_mix(fg, bg, d ? 0.38 : 0.45);
    palette[C_TEXT4] = rgba_mix(fg, bg, d ? 0.55 : 0.62);
    palette[C_ACCENT] = accent;
    palette[C_ACCENT_SOFT] = rgba_alpha(accent, d ? 0.16 : 0.10);
    palette[C_ON_ACCENT] = luminance(accent) > 0.6 ? rgb(20, 20, 20, 1) : rgb(255, 255, 255, 1);
    palette[C_USER_BUBBLE] = d ? rgba_mix(bg, fg, 0.10) : rgba_mix(bg, accent, 0.10);
    palette[C_CODE_BG] = rgba_mix(bg, fg, d ? 0.055 : 0.025);
    palette[C_CODE_HEADER] = rgba_mix(bg, fg, d ? 0.085 : 0.045);
    palette[C_CODE_BORDER] = rgba_alpha(fg, d ? 0.09 : 0.07);
    palette[C_INLINE_CODE_BG] = rgba_alpha(fg, d ? 0.10 : 0.055);
    palette[C_SEPARATOR] = rgba_alpha(fg, d ? 0.11 : 0.09);
    palette[C_HOVER] = rgba_alpha(fg, d ? 0.07 : 0.045);
    palette[C_PRESSED] = rgba_alpha(fg, d ? 0.12 : 0.08);
    palette[C_CARD_BG] = d ? rgba_mix(bg, fg, 0.06) : base;
    palette[C_CARD_BORDER] = rgba_alpha(fg, d ? 0.13 : 0.11);
    palette[C_SELECTION] = rgba_alpha(accent, d ? 0.38 : 0.26);
    palette[C_DIFF_ADD] = dyn(d, rgb(34, 160, 80, 0.12), rgb(60, 190, 110, 0.16));
    palette[C_DIFF_ADD_TEXT] = dyn(d, rgb(22, 128, 60, 1), rgb(110, 210, 140, 1));
    palette[C_DIFF_REM] = dyn(d, rgb(220, 50, 50, 0.10), rgb(240, 90, 90, 0.16));
    palette[C_DIFF_REM_TEXT] = dyn(d, rgb(190, 40, 40, 1), rgb(250, 130, 130, 1));
    palette[C_ERROR] = dyn(d, rgb(212, 56, 56, 1), rgb(255, 120, 120, 1));
    palette[C_ERROR_SOFT] = dyn(d, rgb(212, 56, 56, 0.08), rgb(255, 120, 120, 0.12));
    palette[C_WARNING] = dyn(d, rgb(196, 124, 0, 1), rgb(255, 190, 80, 1));
    palette[C_WARNING_SOFT] = dyn(d, rgb(255, 170, 0, 0.10), rgb(255, 190, 80, 0.12));
    palette[C_SUCCESS] = dyn(d, rgb(34, 150, 80, 1), rgb(100, 210, 140, 1));
    palette[C_SYN_KEYWORD] = dyn(d, rgb(207, 34, 46, 1), rgb(255, 123, 114, 1));
    palette[C_SYN_STRING] = dyn(d, rgb(10, 48, 105, 1), rgb(165, 214, 255, 1));
    palette[C_SYN_COMMENT] = dyn(d, rgb(110, 119, 129, 1), rgb(139, 148, 158, 1));
    palette[C_SYN_NUMBER] = dyn(d, rgb(5, 80, 174, 1), rgb(121, 192, 255, 1));
    palette[C_SYN_TYPE] = dyn(d, rgb(149, 56, 0, 1), rgb(255, 166, 87, 1));
    palette[C_SYN_FUNCTION] = dyn(d, rgb(130, 80, 223, 1), rgb(210, 168, 255, 1));
    palette[C_WHITE] = rgb(255, 255, 255, 1);
    palette[C_CLEAR] = rgb(0, 0, 0, 0);
}

static char *read_mono_family(void) {
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema = src ? g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE) : NULL;
    char *fam = NULL;
    if (schema) {
        if (g_settings_schema_has_key(schema, "monospace-font-name")) {
            GSettings *s = g_settings_new("org.gnome.desktop.interface");
            char *v = g_settings_get_string(s, "monospace-font-name");
            if (v && *v) {
                PangoFontDescription *d = pango_font_description_from_string(v);
                if (pango_font_description_get_family(d)) fam = g_strdup(pango_font_description_get_family(d));
                pango_font_description_free(d);
            }
            g_free(v);
            g_object_unref(s);
        }
        g_settings_schema_unref(schema);
    }
    return fam ? fam : g_strdup("Monospace");
}

static void build_fonts(void) {
    for (int i = 0; i < N_FONTS; i++)
        if (fonts[i]) pango_font_description_free(fonts[i]);
    struct { FontId id; double scale; PangoWeight w; gboolean mono; } spec[] = {
        { F_BODY, 1.0, PANGO_WEIGHT_NORMAL, FALSE },     { F_SMALL, 0.93, PANGO_WEIGHT_NORMAL, FALSE },
        { F_CAPTION, 0.86, PANGO_WEIGHT_NORMAL, FALSE }, { F_TINY, 0.79, PANGO_WEIGHT_NORMAL, FALSE },
        { F_MONO, 0.875, PANGO_WEIGHT_NORMAL, TRUE },    { F_MONO_SMALL, 0.82, PANGO_WEIGHT_NORMAL, TRUE },
        { F_THINKING, 0.93, PANGO_WEIGHT_NORMAL, FALSE }, { F_H1, 1.57, PANGO_WEIGHT_SEMIBOLD, FALSE },
        { F_H2, 1.36, PANGO_WEIGHT_SEMIBOLD, FALSE },    { F_H3, 1.18, PANGO_WEIGHT_SEMIBOLD, FALSE },
        { F_H4, 1.04, PANGO_WEIGHT_SEMIBOLD, FALSE },    { F_TITLE, 1.86, PANGO_WEIGHT_SEMIBOLD, FALSE },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(spec); i++) {
        PangoFontDescription *d = pango_font_description_new();
        pango_font_description_set_family(d, spec[i].mono ? mono_family : ui_family);
        pango_font_description_set_size(d, (int)(base_pt * spec[i].scale * PANGO_SCALE + 0.5));
        pango_font_description_set_weight(d, spec[i].w);
        fonts[spec[i].id] = d;
    }
}

gboolean theme_update(GtkWidget *w) {
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    GtkSettings *gs = gtk_widget_get_settings(w);
    RGBA bg = rgb(255, 255, 255, 1), fg = rgb(15, 17, 21, 1), accent = rgb(65, 118, 230, 1), base = bg, sidebar;
    if (!lookup(sc, "theme_bg_color", &bg)) lookup(sc, "window_bg_color", &bg);
    if (!lookup(sc, "theme_fg_color", &fg)) lookup(sc, "window_fg_color", &fg);
    if (!lookup(sc, "theme_selected_bg_color", &accent)) lookup(sc, "accent_bg_color", &accent);
    if (!lookup(sc, "theme_base_color", &base)) base = bg;
    bg.a = 1;
    gboolean d = luminance(bg) < 0.45;
    const char *force = g_getenv("DSN_APPEARANCE");
    if (!force && forced_appearance) force = forced_appearance == 2 ? "dark" : "light";
    if (force && !strcmp(force, "dark") && !d) { bg = rgb(21, 21, 23, 1); fg = rgb(249, 250, 251, 1); base = rgb(34, 34, 36, 1); d = TRUE; }
    if (force && !strcmp(force, "light") && d) { bg = rgb(255, 255, 255, 1); fg = rgb(15, 17, 21, 1); base = bg; d = FALSE; }
    /* A very grey accent reads poorly as a link color; fall back to the harness blue. */
    double sat = MAX(MAX(accent.r, accent.g), accent.b) - MIN(MIN(accent.r, accent.g), accent.b);
    if (sat < 0.12) accent = d ? rgb(122, 170, 255, 1) : rgb(65, 118, 230, 1);
    /* Accents tuned for selection backgrounds can be too dark to read as text on a dark canvas. */
    if (d && luminance(accent) < 0.38) accent = rgba_mix(accent, rgb(255, 255, 255, 1), 0.38);
    if (!lookup(sc, "sidebar_bg_color", &sidebar)) sidebar = rgba_mix(bg, fg, d ? 0.035 : 0.03);
    sidebar.a = 1;
    char *fontname = NULL;
    g_object_get(gs, "gtk-font-name", &fontname, NULL);
    PangoFontDescription *fd = pango_font_description_from_string(fontname ? fontname : "Sans 11");
    g_free(fontname);
    char *family = g_strdup(pango_font_description_get_family(fd) ? pango_font_description_get_family(fd) : "Sans");
    double pt = pango_font_description_get_size(fd) / (double)PANGO_SCALE;
    if (pt <= 0) pt = 11;
    pango_font_description_free(fd);
    char *mono = read_mono_family();
    RGBA old_bg = palette[C_BG], old_fg = palette[C_TEXT], old_acc = palette[C_ACCENT];
    gboolean changed = !ui_family || strcmp(family, ui_family) || fabs(pt - base_pt) > 0.01 || strcmp(mono, mono_family ? mono_family : "") ||
                       memcmp(&old_bg, &bg, sizeof bg) || memcmp(&old_fg, &fg, sizeof fg) || memcmp(&old_acc, &accent, sizeof accent) || d != dark;
    if (!changed) { g_free(family); g_free(mono); return FALSE; }
    g_free(ui_family);
    ui_family = family;
    g_free(mono_family);
    mono_family = mono;
    base_pt = pt;
    dark = d;
    build_palette(bg, fg, accent, base, sidebar);
    build_fonts();
    generation++;
    if (icon_cache) g_hash_table_remove_all(icon_cache);
    if (width_cache) g_hash_table_remove_all(width_cache);
    return TRUE;
}

gboolean theme_dark(void) { return dark; }
guint theme_generation(void) { return generation; }

RGBA th(ColorId c) {
    if (palette[C_TEXT].a == 0) { dark = FALSE; build_palette(rgb(255, 255, 255, 1), rgb(15, 17, 21, 1), rgb(65, 118, 230, 1), rgb(255, 255, 255, 1), rgb(247, 247, 248, 1)); }
    return palette[c];
}

void set_rgba(cairo_t *cr, RGBA c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }
void set_color(cairo_t *cr, ColorId c) { set_rgba(cr, th(c)); }

char *rgba_css(RGBA c) {
    char a[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(a, sizeof a, "%.3f", c.a);
    return g_strdup_printf("rgba(%d,%d,%d,%s)", (int)lround(c.r * 255), (int)lround(c.g * 255), (int)lround(c.b * 255), a);
}

const PangoFontDescription *theme_font(FontId f) {
    if (!fonts[F_BODY]) {
        ui_family = g_strdup("Sans");
        mono_family = read_mono_family();
        build_fonts();
    }
    return fonts[f];
}

PangoFontDescription *theme_font_styled(FontId f, int flags) {
    PangoFontDescription *d = pango_font_description_copy(theme_font(f));
    if (flags & FS_MONO) {
        pango_font_description_set_family(d, mono_family);
        pango_font_description_set_size(d, (int)(pango_font_description_get_size(d) * 0.9));
    }
    if (flags & FS_BOLD) pango_font_description_set_weight(d, PANGO_WEIGHT_SEMIBOLD);
    else if (flags & FS_SEMIBOLD) pango_font_description_set_weight(d, PANGO_WEIGHT_SEMIBOLD);
    else if (flags & FS_MEDIUM) pango_font_description_set_weight(d, PANGO_WEIGHT_MEDIUM);
    if (flags & FS_ITALIC) pango_font_description_set_style(d, PANGO_STYLE_ITALIC);
    return d;
}

double theme_font_px(FontId f) { return pango_font_description_get_size(theme_font(f)) / (double)PANGO_SCALE * 96.0 / 72.0; }
const char *theme_mono_family(void) { theme_font(F_BODY); return mono_family; }

PangoContext *theme_pango(void) {
    if (!pango_ctx) {
        PangoFontMap *fm = pango_cairo_font_map_get_default();
        pango_ctx = pango_font_map_create_context(fm);
        pango_cairo_context_set_resolution(pango_ctx, 96);
    }
    return pango_ctx;
}

void theme_set_pango(PangoContext *ctx) {
    if (ctx == pango_ctx) return;
    if (pango_ctx) g_object_unref(pango_ctx);
    pango_ctx = g_object_ref(ctx);
    generation++;
    if (width_cache) g_hash_table_remove_all(width_cache);
}

void theme_set_scale(int scale) {
    if (scale == ui_scale) return;
    ui_scale = scale;
    if (icon_cache) g_hash_table_remove_all(icon_cache);
}

/* ---------------- drawing ---------------- */

void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    r = MIN(r, MIN(w, h) / 2);
    if (r <= 0) { cairo_rectangle(cr, x, y, w, h); return; }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

void fill_rounded(cairo_t *cr, double x, double y, double w, double h, double r, RGBA c) {
    if (c.a <= 0) return;
    rounded_rect(cr, x, y, w, h, r);
    set_rgba(cr, c);
    cairo_fill(cr);
}

void stroke_rounded(cairo_t *cr, double x, double y, double w, double h, double r, RGBA c, double lw) {
    rounded_rect(cr, x + lw / 2, y + lw / 2, w - lw, h - lw, r);
    set_rgba(cr, c);
    cairo_set_line_width(cr, lw);
    cairo_stroke(cr);
}

void draw_spinner(cairo_t *cr, double cx, double cy, double radius, double phase, RGBA c) {
    cairo_save(cr);
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    set_rgba(cr, rgba_alpha(c, 0.2));
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI);
    cairo_stroke(cr);
    set_rgba(cr, c);
    double a = phase * 2 * G_PI;
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, radius, a, a + G_PI * 0.6);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void draw_chevron(cairo_t *cr, double cx, double cy, double s, int dir, RGBA c) {
    cairo_save(cr);
    set_rgba(cr, c);
    cairo_set_line_width(cr, 1.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    double h = s / 2, q = s / 4;
    if (dir == 0) { cairo_move_to(cr, cx - h, cy - q); cairo_line_to(cr, cx, cy + q); cairo_line_to(cr, cx + h, cy - q); }
    else if (dir == 1) { cairo_move_to(cr, cx - h, cy + q); cairo_line_to(cr, cx, cy - q); cairo_line_to(cr, cx + h, cy + q); }
    else { cairo_move_to(cr, cx - q, cy - h); cairo_line_to(cr, cx + q, cy); cairo_line_to(cr, cx - q, cy + h); }
    cairo_stroke(cr);
    cairo_restore(cr);
}

void draw_check(cairo_t *cr, double x, double y, double w, double h, RGBA c, double lw) {
    cairo_save(cr);
    set_rgba(cr, c);
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, x + w * 0.22, y + h * 0.52);
    cairo_line_to(cr, x + w * 0.42, y + h * 0.72);
    cairo_line_to(cr, x + w * 0.78, y + h * 0.30);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static cairo_surface_t *icon_surface(const char *name, int size, RGBA c) {
    if (!icon_cache) icon_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)cairo_surface_destroy);
    char *key = g_strdup_printf("%s|%d|%d|%.3f,%.3f,%.3f,%.3f", name, size, ui_scale, c.r, c.g, c.b, c.a);
    cairo_surface_t *s = g_hash_table_lookup(icon_cache, key);
    if (s || g_hash_table_contains(icon_cache, key)) { g_free(key); return s; }
    GtkIconTheme *it = gtk_icon_theme_get_default();
    GtkIconInfo *info = gtk_icon_theme_lookup_icon_for_scale(it, name, size, ui_scale, GTK_ICON_LOOKUP_FORCE_SYMBOLIC | GTK_ICON_LOOKUP_FORCE_SIZE);
    if (info) {
        GdkRGBA fg = { c.r, c.g, c.b, c.a };
        GdkPixbuf *pb = gtk_icon_info_load_symbolic(info, &fg, NULL, NULL, NULL, NULL, NULL);
        if (pb) {
            s = gdk_cairo_surface_create_from_pixbuf(pb, ui_scale, NULL);
            g_object_unref(pb);
        }
        g_object_unref(info);
    }
    if (g_hash_table_size(icon_cache) > 600) g_hash_table_remove_all(icon_cache);
    g_hash_table_insert(icon_cache, key, s);
    return s;
}

void draw_icon(cairo_t *cr, const char *name, double x, double y, double w, double h, int size, RGBA c) {
    cairo_surface_t *s = icon_surface(name, size, c);
    if (!s) {
        cairo_save(cr);
        set_rgba(cr, c);
        cairo_new_path(cr);
        cairo_arc(cr, x + w / 2, y + h / 2, size / 5.0, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_restore(cr);
        return;
    }
    double iw = cairo_image_surface_get_width(s) / (double)ui_scale, ih = cairo_image_surface_get_height(s) / (double)ui_scale;
    cairo_save(cr);
    cairo_set_source_surface(cr, s, round(x + (w - iw) / 2), round(y + (h - ih) / 2));
    cairo_paint(cr);
    cairo_restore(cr);
}

/* DeepSeek whale logo from the harness favicon path (viewBox 50×50). */
static const char *LOGO =
    "M48.8354 10.0479C48.3232 9.79199 48.1025 10.2798 47.8032 10.5278C47.7007 10.6079 47.6143 10.7119 47.5273 10.8076C46.7793 11.624 45.9048 12.1597 44.7622 12.0957C43.0923 12 41.666 12.5356 40.4058 13.8398C40.1377 12.2319 39.2476 11.272 37.8926 10.6558C37.1836 10.3359 36.4668 10.0156 35.9702 9.31982C35.6235 8.82373 35.5293 8.27197 35.356 7.72754C35.2456 7.3999 35.1353 7.06396 34.7651 7.00781C34.3633 6.94385 34.2056 7.2876 34.0479 7.57568C33.418 8.75195 33.1733 10.0479 33.1973 11.3599C33.2524 14.312 34.4736 16.6641 36.8999 18.3359C37.1758 18.5278 37.2466 18.7197 37.1597 19C36.9946 19.5757 36.7974 20.1357 36.624 20.7119C36.5137 21.0801 36.3486 21.1597 35.9624 21C34.6309 20.4321 33.481 19.5918 32.4644 18.5757C30.7393 16.8721 29.1792 14.9917 27.2334 13.52C26.7764 13.1758 26.3193 12.856 25.8467 12.5518C23.8618 10.584 26.1069 8.96777 26.627 8.77588C27.1704 8.57568 26.8159 7.8877 25.0591 7.896C23.3022 7.90381 21.6953 8.50391 19.647 9.30371C19.3477 9.42383 19.0322 9.51172 18.7095 9.58398C16.8501 9.22363 14.9199 9.14355 12.9033 9.37598C9.10596 9.80762 6.07275 11.6396 3.84326 14.7681C1.16455 18.5278 0.53418 22.7998 1.30664 27.2559C2.11768 31.9521 4.46582 35.8398 8.07373 38.8799C11.8159 42.0322 16.1255 43.5762 21.041 43.2803C24.0269 43.104 27.3516 42.6963 31.1016 39.4561C32.0469 39.936 33.0396 40.1279 34.686 40.272C35.9546 40.3921 37.1758 40.208 38.1211 40.0078C39.6021 39.688 39.4995 38.2881 38.9639 38.0322C34.623 35.9678 35.5762 36.8081 34.71 36.1279C36.9155 33.4639 40.2402 30.6958 41.54 21.728C41.6426 21.0161 41.5557 20.5679 41.54 19.9917C41.5322 19.6396 41.6108 19.5039 42.0049 19.4639C43.0923 19.3359 44.1479 19.0317 45.1167 18.4878C47.9292 16.9199 49.064 14.3438 49.3315 11.2559C49.3711 10.7837 49.3237 10.2959 48.8354 10.0479ZM24.3262 37.8398C20.1196 34.4639 18.0791 33.3521 17.2358 33.3999C16.4482 33.4482 16.5898 34.3682 16.7632 34.9678C16.9443 35.5601 17.1812 35.9683 17.5117 36.4878C17.7402 36.832 17.8979 37.3442 17.2832 37.728C15.9282 38.584 13.5728 37.4399 13.4624 37.3838C10.7207 35.7358 8.42822 33.5601 6.81348 30.584C5.25342 27.7197 4.34766 24.6479 4.19775 21.3677C4.1582 20.5757 4.38672 20.2959 5.15869 20.1519C6.17529 19.96 7.22314 19.9199 8.23926 20.0718C12.5327 20.7119 16.1885 22.6719 19.2529 25.7759C21.002 27.5439 22.3252 29.6558 23.6885 31.7202C25.1377 33.9121 26.6978 36 28.6831 37.7119C29.3843 38.312 29.9434 38.7681 30.479 39.104C28.8643 39.2881 26.1699 39.3281 24.3262 37.8398ZM26.3433 24.6001C26.3433 24.248 26.6191 23.9678 26.9658 23.9678C27.0444 23.9678 27.1152 23.9839 27.1782 24.0078C27.2651 24.04 27.3438 24.0879 27.4067 24.1602C27.5171 24.272 27.5801 24.4321 27.5801 24.6001C27.5801 24.9521 27.3042 25.2319 26.9575 25.2319C26.6108 25.2319 26.3433 24.9521 26.3433 24.6001ZM32.6064 27.8799C32.2046 28.0479 31.8027 28.1919 31.4165 28.208C30.8179 28.2397 30.1641 27.9922 29.8096 27.688C29.2583 27.2158 28.8643 26.9521 28.6987 26.1279C28.6279 25.7759 28.6675 25.2319 28.7305 24.9199C28.8721 24.248 28.7144 23.8159 28.2495 23.4238C27.8716 23.104 27.3911 23.0161 26.8633 23.0161C26.666 23.0161 26.4849 22.9277 26.3511 22.856C26.1304 22.7441 25.9492 22.4639 26.1226 22.1201C26.1777 22.0078 26.4458 21.7358 26.5088 21.688C27.2256 21.272 28.0527 21.4077 28.8169 21.7197C29.5259 22.0161 30.0615 22.5601 30.834 23.3281C31.6216 24.2559 31.7632 24.5117 32.2124 25.208C32.5669 25.752 32.8901 26.312 33.1104 26.9521C33.2446 27.3521 33.0713 27.6802 32.6064 27.8799Z";

static cairo_path_t *logo_path;

static void logo_flush(cairo_t *cr, char cmd, GArray *nums, double *cx, double *cy) {
    double *n = (double *)nums->data;
    guint c = nums->len;
    switch (cmd) {
    case 'M':
        for (guint i = 0; i + 1 < c; i += 2) {
            if (i == 0) cairo_move_to(cr, n[i], n[i + 1]); else cairo_line_to(cr, n[i], n[i + 1]);
            *cx = n[i]; *cy = n[i + 1];
        }
        break;
    case 'L':
        for (guint i = 0; i + 1 < c; i += 2) { cairo_line_to(cr, n[i], n[i + 1]); *cx = n[i]; *cy = n[i + 1]; }
        break;
    case 'H': for (guint i = 0; i < c; i++) { *cx = n[i]; cairo_line_to(cr, *cx, *cy); } break;
    case 'V': for (guint i = 0; i < c; i++) { *cy = n[i]; cairo_line_to(cr, *cx, *cy); } break;
    case 'C':
        for (guint i = 0; i + 5 < c; i += 6) { cairo_curve_to(cr, n[i], n[i + 1], n[i + 2], n[i + 3], n[i + 4], n[i + 5]); *cx = n[i + 4]; *cy = n[i + 5]; }
        break;
    case 'Z': case 'z': cairo_close_path(cr); break;
    }
    g_array_set_size(nums, 0);
}

static void build_logo(void) {
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t *cr = cairo_create(s);
    GArray *nums = g_array_new(FALSE, FALSE, sizeof(double));
    char cmd = 'M';
    double cx = 0, cy = 0;
    char tok[64];
    int tl = 0;
#define PUSH() do { if (tl) { tok[tl] = 0; double v = g_ascii_strtod(tok, NULL); g_array_append_val(nums, v); tl = 0; } } while (0)
    for (const char *p = LOGO; *p; p++) {
        char ch = *p;
        if (g_ascii_isalpha(ch) && ch != 'e') {
            PUSH();
            logo_flush(cr, cmd, nums, &cx, &cy);
            cmd = ch;
            if (ch == 'Z' || ch == 'z') logo_flush(cr, cmd, nums, &cx, &cy);
        } else if (ch == '-' && tl && tok[tl - 1] != 'e') {
            PUSH();
            tok[tl++] = '-';
        } else if (ch == ' ' || ch == ',') {
            PUSH();
        } else if (ch == '.' && memchr(tok, '.', tl)) {
            PUSH();
            tok[tl++] = '.';
        } else if (tl < 63) {
            tok[tl++] = ch;
        }
    }
    PUSH();
    logo_flush(cr, cmd, nums, &cx, &cy);
#undef PUSH
    logo_path = cairo_copy_path(cr);
    g_array_unref(nums);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
}

void draw_logo(cairo_t *cr, double x, double y, double size, RGBA c) {
    if (!logo_path) build_logo();
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, size / 50.0, size / 50.0);
    cairo_new_path(cr);
    cairo_append_path(cr, logo_path);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    set_rgba(cr, c);
    cairo_fill(cr);
    cairo_restore(cr);
}

GdkPixbuf *theme_logo_pixbuf(int size, RGBA c) {
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(s);
    draw_logo(cr, 0, 0, size, c);
    cairo_destroy(cr);
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(s, 0, 0, size, size);
    cairo_surface_destroy(s);
    return pb;
}

static PangoLayout *label_layout(const char *text, FontId f, int flags) {
    PangoLayout *l = pango_layout_new(theme_pango());
    PangoFontDescription *d = theme_font_styled(f, flags);
    pango_layout_set_font_description(l, d);
    pango_font_description_free(d);
    pango_layout_set_text(l, text ? text : "", -1);
    pango_layout_set_single_paragraph_mode(l, TRUE);
    return l;
}

double text_width(const char *text, FontId f, int flags) {
    if (!width_cache) width_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char *key = g_strdup_printf("%d|%d|%s", f, flags, text ? text : "");
    double *v = g_hash_table_lookup(width_cache, key);
    if (v) { g_free(key); return *v; }
    PangoLayout *l = label_layout(text, f, flags);
    int w, h;
    pango_layout_get_pixel_size(l, &w, &h);
    g_object_unref(l);
    if (g_hash_table_size(width_cache) > 4000) g_hash_table_remove_all(width_cache);
    v = g_new(double, 1);
    *v = w;
    g_hash_table_insert(width_cache, key, v);
    return w;
}

double draw_label(cairo_t *cr, const char *text, double x, double y, double w, double h, FontId f, int flags, RGBA c, PangoAlignment align) {
    if (!text || !*text || w <= 1) return 0;
    PangoLayout *l = label_layout(text, f, flags);
    pango_layout_set_width(l, (int)(w * PANGO_SCALE));
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(l, align);
    if (flags & FS_STRIKE) {
        PangoAttrList *al = pango_attr_list_new();
        pango_attr_list_insert(al, pango_attr_strikethrough_new(TRUE));
        pango_layout_set_attributes(l, al);
        pango_attr_list_unref(al);
    }
    PangoRectangle ink, log;
    pango_layout_get_pixel_extents(l, &ink, &log);
    cairo_save(cr);
    set_rgba(cr, c);
    cairo_move_to(cr, x, round(y + (h - log.height) / 2.0));
    pango_cairo_update_layout(cr, l);
    pango_cairo_show_layout(cr, l);
    cairo_restore(cr);
    g_object_unref(l);
    return MIN(log.width, w);
}

void ui_show_uri(GtkWidget *w, const char *uri) {
    if (!uri) return;
    if (g_getenv("DSN_NO_OPEN")) {
        fprintf(stderr, "[dsn] open suppressed: %s\n", uri);
        return;
    }
    GError *err = NULL;
    GtkWidget *top = w ? gtk_widget_get_toplevel(w) : NULL;
    gtk_show_uri_on_window(top && GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : NULL, uri, GDK_CURRENT_TIME, &err);
    if (err) {
        dlog("open %s failed: %s", uri, err->message);
        g_error_free(err);
    }
}
