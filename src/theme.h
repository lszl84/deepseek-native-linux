#pragma once
#include <gtk/gtk.h>

typedef struct { double r, g, b, a; } RGBA;

typedef enum {
    C_TEXT, C_TEXT2, C_TEXT3, C_TEXT4, C_ACCENT, C_ACCENT_SOFT, C_ON_ACCENT, C_BG, C_SIDEBAR_BG, C_USER_BUBBLE,
    C_CODE_BG, C_CODE_HEADER, C_CODE_BORDER, C_INLINE_CODE_BG, C_SEPARATOR, C_HOVER, C_PRESSED,
    C_CARD_BG, C_CARD_BORDER, C_SELECTION, C_DIFF_ADD, C_DIFF_ADD_TEXT, C_DIFF_REM, C_DIFF_REM_TEXT,
    C_ERROR, C_ERROR_SOFT, C_WARNING, C_WARNING_SOFT, C_SUCCESS,
    C_SYN_KEYWORD, C_SYN_STRING, C_SYN_COMMENT, C_SYN_NUMBER, C_SYN_TYPE, C_SYN_FUNCTION,
    C_WHITE, C_CLEAR, N_COLORS
} ColorId;

typedef enum {
    F_BODY, F_SMALL, F_CAPTION, F_TINY, F_MONO, F_MONO_SMALL, F_THINKING, F_H1, F_H2, F_H3, F_H4, F_TITLE, N_FONTS
} FontId;

enum { FS_BOLD = 1, FS_ITALIC = 2, FS_MONO = 4, FS_STRIKE = 8, FS_CODE = 16, FS_SEMIBOLD = 32, FS_MEDIUM = 64 };

/* Reads colors and fonts from the GTK theme of `w`. Returns TRUE when anything changed. */
gboolean theme_update(GtkWidget *w);
void theme_force_appearance(int mode); /* 0 follow theme, 1 light, 2 dark */
gboolean theme_dark(void);
guint theme_generation(void);
RGBA th(ColorId c);
RGBA rgba_alpha(RGBA c, double a);
RGBA rgba_mix(RGBA a, RGBA b, double t);
void set_color(cairo_t *cr, ColorId c);
void set_rgba(cairo_t *cr, RGBA c);
char *rgba_css(RGBA c);

/* Fonts: newly computed descriptions are cached; do not free. */
const PangoFontDescription *theme_font(FontId f);
PangoFontDescription *theme_font_styled(FontId f, int flags); /* caller frees */
double theme_font_px(FontId f);
const char *theme_mono_family(void);
PangoContext *theme_pango(void);
void theme_set_pango(PangoContext *ctx);

/* Drawing helpers */
void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r);
void fill_rounded(cairo_t *cr, double x, double y, double w, double h, double r, RGBA c);
void stroke_rounded(cairo_t *cr, double x, double y, double w, double h, double r, RGBA c, double lw);
void draw_spinner(cairo_t *cr, double cx, double cy, double radius, double phase, RGBA c);
void draw_icon(cairo_t *cr, const char *name, double x, double y, double w, double h, int size, RGBA c);
void draw_chevron(cairo_t *cr, double cx, double cy, double size, int dir /* 0 down, 1 up, 2 right */, RGBA c);
void draw_check(cairo_t *cr, double x, double y, double w, double h, RGBA c, double lw);
void draw_logo(cairo_t *cr, double x, double y, double size, RGBA c);
/* Single-line label truncated with an ellipsis; returns the drawn width. */
double draw_label(cairo_t *cr, const char *text, double x, double y, double w, double h, FontId f, int flags, RGBA c, PangoAlignment align);
double text_width(const char *text, FontId f, int flags);
void theme_set_scale(int scale);
GdkPixbuf *theme_logo_pixbuf(int size, RGBA c);

/* Opens a URI with the default handler (suppressed when DSN_NO_OPEN is set). */
void ui_show_uri(GtkWidget *w, const char *uri);
