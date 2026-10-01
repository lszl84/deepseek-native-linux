#include "markdown.h"
#include "util.h"
#include <string.h>

MDStyle md_style_body(const char *base_dir) { return (MDStyle){ F_BODY, 0, C_TEXT, 7, base_dir }; }
MDStyle md_style_thinking(void) { return (MDStyle){ F_THINKING, 0, C_TEXT3, 6, NULL }; }

static void item_free(gpointer p) {
    MDItem *it = p;
    g_ptr_array_unref(it->blocks);
    g_free(it);
}

void md_block_free(gpointer p) {
    MDBlock *b = p;
    if (!b) return;
    rt_free(b->rt);
    g_free(b->lang);
    g_free(b->code);
    if (b->items) g_ptr_array_unref(b->items);
    if (b->children) g_ptr_array_unref(b->children);
    if (b->header) g_ptr_array_unref(b->header);
    if (b->rows) g_ptr_array_unref(b->rows);
    g_free(b->aligns);
    g_free(b->alt);
    g_free(b->src);
    g_free(b);
}

static MDBlock *blk(MDKind k) {
    MDBlock *b = g_new0(MDBlock, 1);
    b->kind = k;
    return b;
}

/* ---------------- line helpers ---------------- */

static int indent_of(const char *s) {
    int n = 0;
    for (; *s; s++) {
        if (*s == ' ') n++;
        else if (*s == '\t') n += 4;
        else break;
    }
    return n;
}

static gboolean is_blank(const char *s) {
    for (; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\r') return FALSE;
    return TRUE;
}

static const char *skip_spaces(const char *s) {
    while (*s == ' ') s++;
    return s;
}

/* Returns TRUE with marker char/count and language for an opening fence. */
static gboolean fence(const char *line, char *mch, int *mcount, char **lang) {
    if (indent_of(line) >= 4) return FALSE;
    const char *t = skip_spaces(line);
    char c = *t;
    if (c != '`' && c != '~') return FALSE;
    int n = 0;
    while (t[n] == c) n++;
    if (n < 3) return FALSE;
    const char *rest = t + n;
    if (c == '`' && strchr(rest, '`')) return FALSE;
    if (mch) *mch = c;
    if (mcount) *mcount = n;
    if (lang) {
        char *r = str_trim(rest);
        char *sp = strpbrk(r, " \t{");
        if (sp) *sp = 0;
        *lang = r;
    }
    return TRUE;
}

static gboolean heading(const char *line, int *level, char **content) {
    if (indent_of(line) >= 4) return FALSE;
    const char *t = skip_spaces(line);
    if (*t != '#') return FALSE;
    int l = 0;
    while (t[l] == '#') l++;
    if (l > 6) return FALSE;
    const char *rest = t + l;
    if (*rest && *rest != ' ' && *rest != '\t') return FALSE;
    if (level) *level = l;
    if (content) {
        char *c = str_trim(rest);
        size_t n = strlen(c);
        while (n > 0 && c[n - 1] == '#') c[--n] = 0;
        char *c2 = str_trim(c);
        g_free(c);
        *content = c2;
    }
    return TRUE;
}

static gboolean is_hr(const char *line) {
    if (indent_of(line) >= 4) return FALSE;
    char f = 0;
    int n = 0;
    for (const char *p = line; *p; p++) {
        if (*p == ' ' || *p == '\t') continue;
        if (!f) {
            if (*p != '-' && *p != '*' && *p != '_') return FALSE;
            f = *p;
        }
        if (*p != f) return FALSE;
        n++;
    }
    return n >= 3;
}

typedef struct { int indent; gboolean ordered; int number; int content_offset; } ListMarker;

static gboolean list_marker(const char *line, ListMarker *m) {
    int ind = indent_of(line);
    const char *t = line;
    while (*t == ' ' || *t == '\t') t++;
    int lead = t - line;
    if (*t == '-' || *t == '*' || *t == '+') {
        const char *after = t + 1;
        if (!*after) { if (m) *m = (ListMarker){ ind, FALSE, 0, lead + 1 }; return TRUE; }
        if (*after == ' ' || *after == '\t') {
            int sp = 0;
            while (after[sp] == ' ') sp++;
            if (sp > 4) sp = 4;
            if (m) *m = (ListMarker){ ind, FALSE, 0, lead + 1 + MAX(1, sp) };
            return TRUE;
        }
        return FALSE;
    }
    int d = 0;
    while (g_ascii_isdigit(t[d])) d++;
    if (d == 0 || d > 9) return FALSE;
    char p = t[d];
    if (p != '.' && p != ')') return FALSE;
    const char *rest = t + d + 1;
    if (*rest && *rest != ' ') return FALSE;
    int sp = 0;
    while (rest[sp] == ' ') sp++;
    if (sp > 4) sp = 4;
    if (m) *m = (ListMarker){ ind, TRUE, atoi(t), lead + d + 1 + MAX(*rest ? 1 : 0, sp) };
    return TRUE;
}

static int *table_separator(const char *line, int *ncols) {
    char *t = str_trim(line);
    if (!strchr(t, '-') || !(strchr(t, '|') || strchr(t, ':') || t[0] == '-')) { g_free(t); return NULL; }
    char *s = t;
    if (*s == '|') s++;
    size_t n = strlen(s);
    if (n && s[n - 1] == '|') s[n - 1] = 0;
    char **cells = g_strsplit(s, "|", -1);
    int count = g_strv_length(cells);
    int *aligns = g_new0(int, MAX(1, count));
    gboolean ok = count > 0;
    for (int i = 0; i < count && ok; i++) {
        char *c = g_strstrip(cells[i]);
        if (!*c) { ok = FALSE; break; }
        gboolean dash = FALSE;
        for (char *p = c; *p; p++) {
            if (*p == '-') dash = TRUE;
            else if (*p != ':') { ok = FALSE; break; }
        }
        if (!dash) ok = FALSE;
        size_t cl = strlen(c);
        gboolean l = c[0] == ':', r = cl && c[cl - 1] == ':';
        aligns[i] = l && r ? PANGO_ALIGN_CENTER : r ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT;
    }
    g_strfreev(cells);
    g_free(t);
    if (!ok) { g_free(aligns); return NULL; }
    *ncols = count;
    return aligns;
}

static GPtrArray *split_row(const char *line) {
    char *t = str_trim(line);
    char *s = t;
    if (*s == '|') s++;
    size_t n = strlen(s);
    if (n && s[n - 1] == '|' && !(n >= 2 && s[n - 2] == '\\')) s[n - 1] = 0;
    GPtrArray *cells = g_ptr_array_new_with_free_func(g_free);
    GString *cur = g_string_new("");
    gboolean in_code = FALSE;
    char prev = ' ';
    for (char *p = s; *p; p++) {
        if (*p == '`') in_code = !in_code;
        if (*p == '|' && !in_code && prev != '\\') {
            g_ptr_array_add(cells, g_string_free(cur, FALSE));
            cur = g_string_new("");
        } else g_string_append_c(cur, *p);
        prev = *p;
    }
    g_ptr_array_add(cells, g_string_free(cur, FALSE));
    for (guint i = 0; i < cells->len; i++) {
        char *c = str_trim(g_ptr_array_index(cells, i));
        char *r = str_replace(c, "\\|", "|");
        g_free(c);
        g_free(cells->pdata[i]);
        cells->pdata[i] = r;
    }
    g_free(t);
    return cells;
}

static gboolean starts_block(const char *line) {
    return fence(line, NULL, NULL, NULL) || heading(line, NULL, NULL) || is_hr(line) || *skip_spaces(line) == '>' || list_marker(line, NULL);
}

/* ---------------- inline ---------------- */

typedef struct {
    FontId font;
    int flags;
    ColorId color;
    const char *link;
} IStyle;

static void flush_buf(RichText *rt, GString *buf, IStyle a) {
    if (!buf->len) return;
    if (a.link) rt_append_link(rt, buf->str, buf->len, a.font, a.flags, a.color, a.link);
    else rt_append(rt, buf->str, buf->len, a.font, a.flags, a.color);
    g_string_truncate(buf, 0);
}

static int find_pat(const gunichar *c, int n, const gunichar *pat, int pl, int from) {
    if (pl <= 0) return -1;
    for (int j = from; j <= n - pl; j++) {
        if (c[j] == '\\') { j++; continue; }
        if (c[j] == pat[0]) {
            gboolean ok = TRUE;
            for (int k = 1; k < pl && ok; k++)
                if (c[j + k] != pat[k]) ok = FALSE;
            if (ok) return j;
        }
    }
    return -1;
}

static char *uni_to_utf8(const gunichar *c, int n) {
    char *s = g_ucs4_to_utf8(c, n, NULL, NULL, NULL);
    return s ? s : g_strdup("");
}

static char *resolve_link(const char *url, const MDStyle *st) {
    if (strstr(url, "://") || g_str_has_prefix(url, "mailto:") || url[0] == '#') return g_strdup(url);
    char *path = g_uri_unescape_string(url, NULL);
    if (!path) path = g_strdup(url);
    char *e = path_expand_tilde(path);
    g_free(path);
    if (e[0] != '/' && st->base_dir) {
        char *j = g_build_filename(st->base_dir, e, NULL);
        g_free(e);
        e = j;
    }
    char *r = g_strconcat("file://", e, NULL);
    g_free(e);
    return r;
}

/* Parses [label](url) starting at i (pointing at '['). */
static gboolean link_at(const gunichar *c, int n, int i, char **label, char **url, int *end, int *label_start, int *label_end) {
    int depth = 0, j = i, le = -1;
    while (j < n) {
        if (c[j] == '\\') { j += 2; continue; }
        if (c[j] == '[') depth++;
        if (c[j] == ']') { depth--; if (depth == 0) { le = j; break; } }
        if (c[j] == '`') {
            int k = j + 1;
            while (k < n && c[k] != '`') k++;
            if (k < n) j = k;
        }
        j++;
    }
    if (le < 0 || le + 1 >= n || c[le + 1] != '(') return FALSE;
    int k = le + 2;
    GString *u = g_string_new("");
    if (k < n && c[k] == '<') {
        k++;
        while (k < n && c[k] != '>') { g_string_append_unichar(u, c[k]); k++; }
        if (k >= n) { g_string_free(u, TRUE); return FALSE; }
        k++;
        while (k < n && c[k] != ')') k++;
        if (k >= n) { g_string_free(u, TRUE); return FALSE; }
    } else {
        int paren = 0;
        while (k < n) {
            if (c[k] == '(') paren++;
            if (c[k] == ')') { if (paren == 0) break; paren--; }
            if (c[k] == ' ') {
                int q = k;
                while (q < n && c[q] != ')') q++;
                k = q;
                break;
            }
            g_string_append_unichar(u, c[k]);
            k++;
        }
        if (k >= n) { g_string_free(u, TRUE); return FALSE; }
    }
    *label = uni_to_utf8(c + i + 1, le - i - 1);
    *label_start = i + 1;
    *label_end = le;
    *url = str_trim(u->str);
    g_string_free(u, TRUE);
    *end = k + 1;
    return TRUE;
}

static gboolean is_letter(gunichar ch) { return g_unichar_isalpha(ch); }
static gboolean is_alnum(gunichar ch) { return g_unichar_isalnum(ch); }

static void append_inline(const gunichar *c, int n, RichText *rt, IStyle a, const MDStyle *st) {
    GString *buf = g_string_new("");
    int i = 0;
    while (i < n) {
        gunichar ch = c[i];
        if (ch == '\\' && i + 1 < n && c[i + 1] < 128 && strchr("\\`*_{}[]()#+-.!|~<>\"'", (char)c[i + 1])) {
            g_string_append_unichar(buf, c[i + 1]);
            i += 2;
            continue;
        }
        /* code span */
        if (ch == '`') {
            int ticks = 0;
            while (i + ticks < n && c[i + ticks] == '`') ticks++;
            int close = -1;
            for (int j = i + ticks; j <= n - ticks; j++) {
                gboolean m = TRUE;
                for (int k = 0; k < ticks && m; k++)
                    if (c[j + k] != '`') m = FALSE;
                if (m && (j + ticks == n || c[j + ticks] != '`')) { close = j; break; }
            }
            if (close >= 0) {
                flush_buf(rt, buf, a);
                char *code = uni_to_utf8(c + i + ticks, close - i - ticks);
                for (char *p = code; *p; p++)
                    if (*p == '\n') *p = ' ';
                size_t cl = strlen(code);
                char *body = code;
                if (cl > 2 && code[0] == ' ' && code[cl - 1] == ' ') { code[cl - 1] = 0; body = code + 1; }
                rt_append(rt, "\u2009", -1, a.font, a.flags, a.color);
                if (a.link) rt_append_link(rt, body, -1, a.font, a.flags | FS_MONO | FS_CODE, a.color, a.link);
                else rt_append(rt, body, -1, a.font, a.flags | FS_MONO | FS_CODE, a.color);
                rt_append(rt, "\u2009", -1, a.font, a.flags, a.color);
                g_free(code);
                i = close + ticks;
                continue;
            }
            for (int k = 0; k < ticks; k++) g_string_append_c(buf, '`');
            i += ticks;
            continue;
        }
        /* images and links */
        if (ch == '!' && i + 1 < n && c[i + 1] == '[') {
            char *label, *url;
            int end, ls, le;
            if (link_at(c, n, i + 1, &label, &url, &end, &ls, &le)) {
                flush_buf(rt, buf, a);
                char *res = resolve_link(url, st);
                char *disp = g_strconcat("🖼 ", *label ? label : path_base(url), NULL);
                rt_append_link(rt, disp, -1, a.font, a.flags, C_ACCENT, res);
                g_free(disp);
                g_free(res);
                g_free(label);
                g_free(url);
                i = end;
                continue;
            }
        }
        if (ch == '[') {
            char *label, *url;
            int end, ls, le;
            if (link_at(c, n, i, &label, &url, &end, &ls, &le)) {
                flush_buf(rt, buf, a);
                char *res = resolve_link(url, st);
                IStyle la = a;
                la.color = C_ACCENT;
                la.link = res;
                append_inline(c + ls, le - ls, rt, la, st);
                g_free(res);
                g_free(label);
                g_free(url);
                i = end;
                continue;
            }
        }
        /* autolinks <http://...> */
        if (ch == '<') {
            gunichar gt = '>';
            int close = find_pat(c, n, &gt, 1, i + 1);
            if (close > 0) {
                char *inner = uni_to_utf8(c + i + 1, close - i - 1);
                if (g_str_has_prefix(inner, "http://") || g_str_has_prefix(inner, "https://") || g_str_has_prefix(inner, "mailto:")) {
                    flush_buf(rt, buf, a);
                    rt_append_link(rt, inner, -1, a.font, a.flags, C_ACCENT, inner);
                    g_free(inner);
                    i = close + 1;
                    continue;
                }
                g_free(inner);
            }
        }
        /* bare URLs */
        if (ch == 'h' && (i == 0 || !is_letter(c[i - 1])) && n - i > 8 && !a.link) {
            char *pre = uni_to_utf8(c + i, MIN(8, n - i));
            gboolean url = g_str_has_prefix(pre, "https://") || g_str_has_prefix(pre, "http://");
            g_free(pre);
            if (url) {
                int j = i;
                while (j < n && !g_unichar_isspace(c[j]) && c[j] != '<' && c[j] != '>' && c[j] != '"' && c[j] != '`') j++;
                while (j > i && c[j - 1] < 128 && strchr(".,;:!?)]'", (char)c[j - 1])) {
                    if (c[j - 1] == ')') {
                        int open = 0, closep = 0;
                        for (int k = i; k < j; k++) { if (c[k] == '(') open++; if (c[k] == ')') closep++; }
                        if (open >= closep) break;
                    }
                    j--;
                }
                flush_buf(rt, buf, a);
                char *u = uni_to_utf8(c + i, j - i);
                rt_append_link(rt, u, -1, a.font, a.flags, C_ACCENT, u);
                g_free(u);
                i = j;
                continue;
            }
        }
        /* emphasis */
        if (ch == '*' || ch == '_' || ch == '~') {
            int run = 0;
            while (i + run < n && c[i + run] == ch) run++;
            gboolean can_open = i + run < n && !g_unichar_isspace(c[i + run]);
            gboolean intraword = ch == '_' && i > 0 && is_alnum(c[i - 1]);
            if (ch == '~' && run == 2 && can_open) {
                gunichar pat[2] = { '~', '~' };
                int close = find_pat(c, n, pat, 2, i + 2);
                if (close > i + 2) {
                    flush_buf(rt, buf, a);
                    IStyle sa = a;
                    sa.flags |= FS_STRIKE;
                    append_inline(c + i + 2, close - i - 2, rt, sa, st);
                    i = close + 2;
                    continue;
                }
            }
            if (ch != '~' && can_open && !intraword) {
                int r = MIN(run, 3);
                gunichar pat[3] = { ch, ch, ch };
                int j = i + r, close = -1, k;
                while ((k = find_pat(c, n, pat, r, j)) >= 0) {
                    if (k > i + r && !g_unichar_isspace(c[k - 1]) && (ch != '_' || k + r >= n || !is_alnum(c[k + r]))) {
                        if (r == 1 && k + 1 < n && c[k + 1] == ch && !(k + 2 < n && c[k + 2] == ch)) { j = k + 2; continue; }
                        close = k;
                        break;
                    }
                    j = k + 1;
                }
                if (close >= 0) {
                    flush_buf(rt, buf, a);
                    IStyle sa = a;
                    if (r == 1) sa.flags |= FS_ITALIC;
                    else if (r == 2) sa.flags |= FS_BOLD;
                    else sa.flags |= FS_BOLD | FS_ITALIC;
                    append_inline(c + i + r, close - i - r, rt, sa, st);
                    i = close + r;
                    continue;
                }
            }
            for (int k2 = 0; k2 < run; k2++) g_string_append_unichar(buf, ch);
            i += run;
            continue;
        }
        g_string_append_unichar(buf, ch);
        i++;
    }
    flush_buf(rt, buf, a);
    g_string_free(buf, TRUE);
}

RichText *md_inline(const char *text, const MDStyle *st) {
    RichText *rt = rt_new();
    glong n = 0;
    gunichar *c = g_utf8_to_ucs4_fast(text ? text : "", -1, &n);
    IStyle a = { st->font, st->flags, st->color, NULL };
    append_inline(c, n, rt, a, st);
    g_free(c);
    return rt;
}

static MDStyle heading_style(int level, const MDStyle *base) {
    MDStyle s = *base;
    s.font = level == 1 ? F_H1 : level == 2 ? F_H2 : level == 3 ? F_H3 : F_H4;
    s.spacing = 4;
    return s;
}

/* ---------------- blocks ---------------- */

static GPtrArray *parse_blocks(char **lines, int count, int *i, const MDStyle *st);

static gboolean sole_image(const char *s, char **alt, char **src) {
    static GRegex *rx;
    if (!rx) rx = g_regex_new("^!\\[([^\\]]*)\\]\\(\\s*<?([^)>]+?)>?(?:\\s+\"[^\"]*\")?\\s*\\)$", 0, 0, NULL);
    GMatchInfo *mi;
    gboolean ok = g_regex_match(rx, s, 0, &mi);
    if (ok) {
        *alt = g_match_info_fetch(mi, 1);
        *src = g_match_info_fetch(mi, 2);
    }
    g_match_info_free(mi);
    return ok;
}

static MDBlock *parse_list(char **lines, int count, int *i, ListMarker first, const MDStyle *st) {
    MDBlock *b = blk(MD_LIST);
    b->ordered = first.ordered;
    b->start = first.number;
    b->items = g_ptr_array_new_with_free_func(item_free);
    int base = first.indent;
    while (*i < count) {
        ListMarker m;
        if (!list_marker(lines[*i], &m) || m.ordered != first.ordered || m.indent < base || !(m.indent < base + 2 || m.indent == base)) break;
        GPtrArray *il = g_ptr_array_new_with_free_func(g_free);
        size_t ll = strlen(lines[*i]);
        g_ptr_array_add(il, g_strdup(lines[*i] + MIN((size_t)m.content_offset, ll)));
        (*i)++;
        gboolean saw_blank = FALSE;
        while (*i < count) {
            const char *l = lines[*i];
            if (is_blank(l)) { saw_blank = TRUE; g_ptr_array_add(il, g_strdup("")); (*i)++; continue; }
            int ind = indent_of(l);
            if (ind >= m.content_offset || (ind > base && list_marker(l, NULL))) {
                int drop = MIN(ind, m.content_offset);
                /* drop leading whitespace columns (tabs count as 4) */
                const char *p = l;
                int col = 0;
                while (*p && col < drop && (*p == ' ' || *p == '\t')) { col += *p == '\t' ? 4 : 1; p++; }
                g_ptr_array_add(il, g_strdup(p));
                saw_blank = FALSE;
                (*i)++;
            } else if (!saw_blank && !list_marker(l, NULL) && !starts_block(l)) {
                g_ptr_array_add(il, str_trim(l));
                (*i)++;
            } else break;
        }
        while (il->len && is_blank(g_ptr_array_index(il, il->len - 1))) g_ptr_array_remove_index(il, il->len - 1);
        MDItem *it = g_new0(MDItem, 1);
        it->task = -1;
        if (il->len) {
            char *f = g_ptr_array_index(il, 0);
            if (g_str_has_prefix(f, "[ ] ") || !strcmp(f, "[ ]")) { it->task = 0; memmove(f, f + MIN(4, strlen(f)), strlen(f + MIN(4, strlen(f))) + 1); }
            else if (g_ascii_strncasecmp(f, "[x] ", 4) == 0 || g_ascii_strcasecmp(f, "[x]") == 0) {
                it->task = 1;
                memmove(f, f + MIN(4, strlen(f)), strlen(f + MIN(4, strlen(f))) + 1);
            }
        }
        g_ptr_array_add(il, NULL);
        int j = 0;
        it->blocks = parse_blocks((char **)il->pdata, il->len - 1, &j, st);
        g_ptr_array_unref(il);
        g_ptr_array_add(b->items, it);
        int k = *i;
        while (k < count && is_blank(lines[k])) k++;
        ListMarker nx;
        if (k < count && list_marker(lines[k], &nx) && nx.ordered == first.ordered && nx.indent == base) *i = k;
        else break;
    }
    return b;
}

static GPtrArray *parse_blocks(char **lines, int count, int *i, const MDStyle *st) {
    GPtrArray *blocks = g_ptr_array_new_with_free_func(md_block_free);
    while (*i < count) {
        const char *line = lines[*i];
        if (is_blank(line)) { (*i)++; continue; }
        char fc;
        int fn;
        char *lang;
        if (fence(line, &fc, &fn, &lang)) {
            int fence_indent = indent_of(line);
            (*i)++;
            GString *code = g_string_new("");
            gboolean first = TRUE;
            while (*i < count) {
                const char *l = lines[*i];
                const char *t = skip_spaces(l);
                int run = 0;
                while (t[run] == fc) run++;
                if (run >= fn && is_blank(t + run)) { (*i)++; break; }
                const char *s = l;
                int drop = fence_indent;
                while (drop > 0 && *s == ' ') { s++; drop--; }
                if (!first) g_string_append_c(code, '\n');
                g_string_append(code, s);
                first = FALSE;
                (*i)++;
            }
            MDBlock *b = blk(MD_CODE);
            b->lang = lang;
            b->code = g_string_free(code, FALSE);
            g_ptr_array_add(blocks, b);
            continue;
        }
        int level;
        char *content;
        if (heading(line, &level, &content)) {
            MDStyle hs = heading_style(level, st);
            MDBlock *b = blk(MD_HEADING);
            b->level = level;
            b->rt = md_inline(content, &hs);
            g_free(content);
            g_ptr_array_add(blocks, b);
            (*i)++;
            continue;
        }
        if (is_hr(line)) { g_ptr_array_add(blocks, blk(MD_HR)); (*i)++; continue; }
        if (*skip_spaces(line) == '>') {
            GPtrArray *inner = g_ptr_array_new_with_free_func(g_free);
            while (*i < count) {
                const char *t = skip_spaces(lines[*i]);
                if (*t == '>') {
                    const char *s = t + 1;
                    if (*s == ' ') s++;
                    g_ptr_array_add(inner, g_strdup(s));
                    (*i)++;
                } else if (!is_blank(lines[*i]) && inner->len && !is_blank(g_ptr_array_index(inner, inner->len - 1)) && !starts_block(lines[*i])) {
                    g_ptr_array_add(inner, g_strdup(lines[*i]));
                    (*i)++;
                } else break;
            }
            MDStyle qs = *st;
            qs.color = C_TEXT2;
            int j = 0;
            int ic = inner->len;
            g_ptr_array_add(inner, NULL);
            MDBlock *b = blk(MD_QUOTE);
            b->children = parse_blocks((char **)inner->pdata, ic, &j, &qs);
            g_ptr_array_unref(inner);
            g_ptr_array_add(blocks, b);
            continue;
        }
        ListMarker lm;
        if (list_marker(line, &lm)) {
            g_ptr_array_add(blocks, parse_list(lines, count, i, lm, st));
            continue;
        }
        int ncols = 0;
        int *aligns = NULL;
        if (*i + 1 < count && strchr(line, '|') && (aligns = table_separator(lines[*i + 1], &ncols))) {
            GPtrArray *header = split_row(line);
            if ((int)header->len == ncols || header->len > 1) {
                *i += 2;
                MDBlock *b = blk(MD_TABLE);
                b->aligns = aligns;
                b->ncols = ncols;
                b->rows = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
                while (*i < count && !is_blank(lines[*i]) && strchr(lines[*i], '|')) {
                    GPtrArray *cells = split_row(lines[*i]);
                    GPtrArray *row = g_ptr_array_new_with_free_func((GDestroyNotify)rt_free);
                    for (guint k = 0; k < cells->len; k++) g_ptr_array_add(row, md_inline(g_ptr_array_index(cells, k), st));
                    g_ptr_array_unref(cells);
                    g_ptr_array_add(b->rows, row);
                    (*i)++;
                }
                MDStyle bold = *st;
                bold.flags |= FS_BOLD;
                b->header = g_ptr_array_new_with_free_func((GDestroyNotify)rt_free);
                for (guint k = 0; k < header->len; k++) g_ptr_array_add(b->header, md_inline(g_ptr_array_index(header, k), &bold));
                g_ptr_array_unref(header);
                g_ptr_array_add(blocks, b);
                continue;
            }
            g_ptr_array_unref(header);
            g_free(aligns);
        }
        /* paragraph */
        GString *para = g_string_new("");
        int nlines = 0;
        while (*i < count) {
            const char *l = lines[*i];
            if (is_blank(l)) break;
            if (nlines) {
                ListMarker m2;
                gboolean lmk = list_marker(l, &m2);
                if (starts_block(l) && (!lmk || !m2.ordered || m2.number == 1)) break;
                int nc;
                int *al;
                if (*i + 1 < count && strchr(l, '|') && (al = table_separator(lines[*i + 1], &nc))) { g_free(al); break; }
            }
            char *t = str_trim(l);
            if (nlines) g_string_append_c(para, '\n');
            g_string_append(para, t);
            size_t ll = strlen(l);
            if (ll >= 2 && l[ll - 1] == ' ' && l[ll - 2] == ' ') g_string_append(para, "\u2028");
            g_free(t);
            nlines++;
            (*i)++;
        }
        char *joined = str_replace(para->str, "\u2028\n", "\u2028");
        g_string_free(para, TRUE);
        char *alt, *src;
        char *tr = str_trim(joined);
        if (g_str_has_prefix(tr, "![") && sole_image(tr, &alt, &src)) {
            MDBlock *b = blk(MD_IMAGE);
            b->alt = alt;
            b->src = src;
            g_ptr_array_add(blocks, b);
        } else {
            MDBlock *b = blk(MD_PARA);
            b->rt = md_inline(joined, st);
            g_ptr_array_add(blocks, b);
        }
        g_free(tr);
        g_free(joined);
    }
    return blocks;
}

GPtrArray *md_parse(const char *text, const MDStyle *st) {
    char *norm = str_replace(text ? text : "", "\r\n", "\n");
    char **lines = g_strsplit(norm, "\n", -1);
    int count = g_strv_length(lines);
    int i = 0;
    GPtrArray *r = parse_blocks(lines, count, &i, st);
    g_strfreev(lines);
    g_free(norm);
    return r;
}

/* ---------------- syntax highlighting ---------------- */

static const char *KEYWORDS[] = {
    "if", "else", "for", "while", "do", "return", "break", "continue", "switch", "case", "default", "in", "of", "true", "false", "null", "nil",
    "none", "None", "True", "False", "undefined", "try", "catch", "finally", "throw", "throws", "import", "export", "from", "as", "new", "class",
    "struct", "enum", "interface", "extends", "implements", "public", "private", "protected", "static", "final", "const", "let", "var", "func",
    "function", "def", "fn", "async", "await", "yield", "self", "this", "super", "type", "typeof", "instanceof", "void", "where", "guard",
    "protocol", "extension", "init", "deinit", "override", "mut", "pub", "impl", "trait", "use", "mod", "crate", "package", "go", "defer",
    "chan", "select", "map", "range", "lambda", "with", "pass", "raise", "except", "elif", "and", "or", "not", "is", "global", "nonlocal", "del",
    "assert", "match", "loop", "unsafe", "move", "ref", "int", "float", "double", "char", "bool", "string", "long", "short", "unsigned",
    "signed", "auto", "inline", "then", "fi", "esac", "done", "local", "echo", "readonly", "unset", "elseif", "end", "begin", "insert",
    "update", "delete", "create", "table", "into", "values", "join", "left", "right", "on", "group", "by", "order", "having", "limit", "SELECT",
    "FROM", "WHERE", "INSERT", "UPDATE", "DELETE", "CREATE", "TABLE", "INTO", "VALUES", "JOIN", "ON", "GROUP", "BY", "ORDER", "LIMIT", "AND",
    "OR", "NOT", "NULL", "AS", "fileprivate", "internal", "open", "some", "any", "inout", "lazy", "weak", "unowned", "mutating", "typealias",
    "associatedtype", "subscript", "get", "set", "willSet", "didSet", "declare", "namespace", "abstract", "typedef", "sizeof", "extern",
    "register", "volatile", "goto", "union", "template", "typename", "virtual", "explicit", "friend", "operator", "noexcept", "constexpr",
    "nullptr", "elif", "fn", "match", "where", NULL
};

static GHashTable *keyword_set(void) {
    static GHashTable *set;
    if (!set) {
        set = g_hash_table_new(g_str_hash, g_str_equal);
        for (int i = 0; KEYWORDS[i]; i++) g_hash_table_add(set, (gpointer)KEYWORDS[i]);
    }
    return set;
}

typedef enum { CS_C, CS_HASH, CS_SQL, CS_NONE } CommentStyle;

static CommentStyle comment_style(const char *lang) {
    const char *hash[] = { "py", "python", "sh", "bash", "zsh", "shell", "console", "yaml", "yml", "toml", "rb", "ruby", "r", "perl", "pl",
                           "dockerfile", "makefile", "make", "conf", "ini", "nix", "elixir", "ex", "cmake", "fish", "ps1", "powershell", NULL };
    const char *sql[] = { "sql", "lua", "haskell", "hs", NULL };
    const char *none[] = { "json", "text", "txt", "plain", "plaintext", "diff", "patch", "md", "markdown", "csv", "log", "output", "", NULL };
    for (int i = 0; hash[i]; i++) if (!g_ascii_strcasecmp(lang, hash[i])) return CS_HASH;
    for (int i = 0; sql[i]; i++) if (!g_ascii_strcasecmp(lang, sql[i])) return CS_SQL;
    for (int i = 0; none[i]; i++) if (!g_ascii_strcasecmp(lang, none[i])) return CS_NONE;
    return CS_C;
}

static gboolean id_start(unsigned char c) { return g_ascii_isalpha(c) || c == '_' || c == '$' || c == '@'; }
static gboolean id_char(unsigned char c) { return id_start(c) || g_ascii_isdigit(c); }

RichText *syntax_highlight(const char *code, const char *lang, FontId font) {
    RichText *rt = rt_new();
    const char *s = code && *code ? code : " ";
    size_t n = strlen(s);
    char *l = g_ascii_strdown(lang ? lang : "", -1);
    if (!strcmp(l, "diff") || !strcmp(l, "patch")) {
        const char *p = s;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);
            ColorId c = C_TEXT;
            if (*p == '+' && strncmp(p, "+++", 3)) c = C_DIFF_ADD_TEXT;
            else if (*p == '-' && strncmp(p, "---", 3)) c = C_DIFF_REM_TEXT;
            else if (!strncmp(p, "@@", 2)) c = C_SYN_FUNCTION;
            rt_append(rt, p, len, font, 0, c);
            p += len;
        }
        g_free(l);
        return rt;
    }
    CommentStyle cs = comment_style(l);
    gboolean json = !strcmp(l, "json");
    if (cs == CS_NONE && !json) {
        rt_append(rt, s, n, font, 0, C_TEXT);
        g_free(l);
        return rt;
    }
    GHashTable *kw = keyword_set();
    size_t i = 0, plain = 0;
#define FLUSH_PLAIN() do { if (i > plain) rt_append(rt, s + plain, i - plain, font, 0, C_TEXT); } while (0)
#define EMIT(st, en, col) do { rt_append(rt, s + (st), (en) - (st), font, 0, col); plain = (en); } while (0)
    while (i < n) {
        unsigned char ch = s[i];
        if (cs == CS_C && ch == '/' && i + 1 < n && (s[i + 1] == '/' || s[i + 1] == '*')) {
            FLUSH_PLAIN();
            size_t st = i;
            if (s[i + 1] == '/') { while (i < n && s[i] != '\n') i++; }
            else {
                i += 2;
                while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
                i = MIN(n, i + 2);
            }
            EMIT(st, i, C_SYN_COMMENT);
            continue;
        }
        if (cs == CS_HASH && ch == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\n' || s[i - 1] == '\t')) {
            FLUSH_PLAIN();
            size_t st = i;
            while (i < n && s[i] != '\n') i++;
            EMIT(st, i, C_SYN_COMMENT);
            continue;
        }
        if (cs == CS_SQL && ch == '-' && i + 1 < n && s[i + 1] == '-') {
            FLUSH_PLAIN();
            size_t st = i;
            while (i < n && s[i] != '\n') i++;
            EMIT(st, i, C_SYN_COMMENT);
            continue;
        }
        if (ch == '"' || ch == '\'' || ch == '`') {
            if (ch == '\'' && i > 0 && id_char(s[i - 1]) && cs != CS_C) { i++; continue; }
            FLUSH_PLAIN();
            unsigned char q = ch;
            size_t st = i;
            i++;
            while (i < n && s[i] != q) {
                if (s[i] == '\\') i++;
                if (i < n && s[i] == '\n' && q != '`') break;
                i++;
            }
            i = MIN(n, i + 1);
            ColorId col = C_SYN_STRING;
            if (json) {
                size_t k = i;
                while (k < n && s[k] == ' ') k++;
                if (k < n && s[k] == ':') col = C_SYN_FUNCTION;
            }
            EMIT(st, i, col);
            continue;
        }
        if (g_ascii_isdigit(ch) && (i == 0 || !id_char(s[i - 1]))) {
            FLUSH_PLAIN();
            size_t st = i;
            while (i < n && (id_char(s[i]) || s[i] == '.')) i++;
            EMIT(st, i, C_SYN_NUMBER);
            continue;
        }
        if (id_start(ch)) {
            size_t st = i;
            while (i < n && id_char(s[i])) i++;
            char *word = g_strndup(s + st, i - st);
            ColorId col = C_TEXT;
            if (g_hash_table_contains(kw, word)) col = C_SYN_KEYWORD;
            else if (i < n && s[i] == '(') col = C_SYN_FUNCTION;
            else if (g_ascii_isupper(word[0]) && i - st > 1) col = C_SYN_TYPE;
            g_free(word);
            if (col != C_TEXT) {
                size_t save = i;
                i = st;
                FLUSH_PLAIN();
                i = save;
                EMIT(st, i, col);
            }
            continue;
        }
        i++;
    }
    FLUSH_PLAIN();
#undef FLUSH_PLAIN
#undef EMIT
    g_free(l);
    return rt;
}
