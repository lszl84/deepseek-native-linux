#define _GNU_SOURCE
#include "util.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

gboolean debug_logging = FALSE;

void dlog(const char *fmt, ...) {
    if (!debug_logging) return;
    va_list ap;
    va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    fprintf(stderr, "[dsn] %s\n", s);
    g_free(s);
}

double now_ts(void) { return g_get_real_time() / 1e6; }

char *uuid_new(void) { return g_uuid_string_random(); }

char *short_uuid(void) {
    char *u = g_uuid_string_random();
    u[8] = 0;
    return u;
}

/* ---------- strings ---------- */

gboolean str_blank(const char *s) {
    if (!s) return TRUE;
    for (; *s; s++)
        if (!g_ascii_isspace(*s)) return FALSE;
    return TRUE;
}

gboolean str_has_prefix(const char *s, const char *p) { return s && p && g_str_has_prefix(s, p); }
gboolean str_has_suffix(const char *s, const char *p) { return s && p && g_str_has_suffix(s, p); }

char *str_trim(const char *s) {
    if (!s) return g_strdup("");
    char *d = g_strdup(s);
    return g_strstrip(d);
}

char *str_replace(const char *s, const char *from, const char *to) {
    if (!s) return g_strdup("");
    if (!from || !*from) return g_strdup(s);
    GString *out = g_string_sized_new(strlen(s));
    size_t fl = strlen(from);
    const char *p = s, *q;
    while ((q = strstr(p, from))) {
        g_string_append_len(out, p, q - p);
        g_string_append(out, to);
        p = q + fl;
    }
    g_string_append(out, p);
    return g_string_free(out, FALSE);
}

int str_count(const char *s, const char *needle) {
    if (!s || !needle || !*needle) return 0;
    int n = 0;
    size_t nl = strlen(needle);
    for (const char *p = strstr(s, needle); p; p = strstr(p + nl, needle)) n++;
    return n;
}

glong utf8_len(const char *s) { return s ? g_utf8_strlen(s, -1) : 0; }

char *utf8_prefix(const char *s, glong n) {
    if (!s) return g_strdup("");
    if (n <= 0) return g_strdup("");
    const char *p = s;
    glong i = 0;
    while (*p && i < n) { p = g_utf8_next_char(p); i++; }
    return g_strndup(s, p - s);
}

char *utf8_suffix(const char *s, glong n) {
    if (!s) return g_strdup("");
    glong len = g_utf8_strlen(s, -1);
    if (len <= n) return g_strdup(s);
    return g_strdup(g_utf8_offset_to_pointer(s, len - n));
}

char *utf8_valid_dup(const char *s, gssize len) {
    if (!s) return g_strdup("");
    if (len < 0) len = strlen(s);
    if (g_utf8_validate(s, len, NULL)) return g_strndup(s, len);
    return g_utf8_make_valid(s, len);
}

char *first_line(const char *s, glong max_chars) {
    if (!s) return g_strdup("");
    const char *nl = strchr(s, '\n');
    char *line = nl ? g_strndup(s, nl - s) : g_strdup(s);
    if (max_chars > 0 && g_utf8_strlen(line, -1) > max_chars) {
        char *p = utf8_prefix(line, max_chars);
        g_free(line);
        line = p;
    }
    return line;
}

int count_lines(const char *s) {
    if (!s) return 0;
    int n = 1;
    for (; *s; s++)
        if (*s == '\n') n++;
    return n;
}

char *ansi_strip(const char *s, gsize len) {
    GString *out = g_string_sized_new(len);
    gsize i = 0;
    gboolean pending_cr = FALSE;
    while (i < len) {
        unsigned char c = s[i];
        if (c == 0x1B) {
            i++;
            if (i >= len) break;
            unsigned char n = s[i++];
            if (n == '[') {
                while (i < len) { unsigned char x = s[i++]; if (x >= 0x40 && x <= 0x7E) break; }
            } else if (n == ']') {
                while (i < len) { unsigned char x = s[i++]; if (x == 0x07 || x == 0x1B) break; }
            }
            continue;
        }
        if (c == '\r') { pending_cr = TRUE; i++; continue; }
        if (pending_cr) {
            pending_cr = FALSE;
            if (c != '\n') {
                /* carriage-return overwrite: drop the current line so far */
                char *nl = memrchr(out->str, '\n', out->len);
                if (nl) g_string_truncate(out, nl - out->str + 1);
                else g_string_truncate(out, 0);
            }
        }
        g_string_append_c(out, c);
        i++;
    }
    char *r = g_string_free(out, FALSE);
    if (!g_utf8_validate(r, -1, NULL)) {
        char *v = g_utf8_make_valid(r, -1);
        g_free(r);
        r = v;
    }
    return r;
}

/* ---------- paths ---------- */

char *path_join(const char *a, const char *b) {
    if (!b || !*b) return g_strdup(a);
    if (b[0] == '/') return g_strdup(b);
    return g_build_filename(a, b, NULL);
}

char *path_standardize(const char *p) {
    if (!p || !*p) return g_strdup("");
    gboolean abs = p[0] == '/';
    char **parts = g_strsplit(p, "/", -1);
    GPtrArray *st = g_ptr_array_new();
    for (int i = 0; parts[i]; i++) {
        char *c = parts[i];
        if (!*c || strcmp(c, ".") == 0) continue;
        if (strcmp(c, "..") == 0) {
            if (st->len > 0 && strcmp(g_ptr_array_index(st, st->len - 1), "..") != 0) g_ptr_array_remove_index(st, st->len - 1);
            else if (!abs) g_ptr_array_add(st, c);
            continue;
        }
        g_ptr_array_add(st, c);
    }
    GString *out = g_string_new(abs ? "/" : "");
    for (guint i = 0; i < st->len; i++) {
        if (i > 0) g_string_append_c(out, '/');
        g_string_append(out, g_ptr_array_index(st, i));
    }
    if (out->len == 0) g_string_append(out, abs ? "/" : ".");
    g_ptr_array_free(st, TRUE);
    g_strfreev(parts);
    return g_string_free(out, FALSE);
}

char *path_expand_tilde(const char *p) {
    if (!p) return g_strdup("");
    if (p[0] == '~' && (p[1] == 0 || p[1] == '/')) return g_strconcat(g_get_home_dir(), p + 1, NULL);
    return g_strdup(p);
}

const char *path_base(const char *p) {
    if (!p) return "";
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') n--;
    const char *s = p + n;
    while (s > p && s[-1] != '/') s--;
    return s;
}

char *path_dirname(const char *p) { return g_path_get_dirname(p); }

char *short_path(const char *p, const char *cwd) {
    if (!p) return g_strdup("");
    if (cwd && *cwd) {
        size_t cl = strlen(cwd);
        if (strncmp(p, cwd, cl) == 0 && p[cl] == '/') return g_strdup(p + cl + 1);
    }
    return tilde_path(p);
}

char *tilde_path(const char *p) {
    const char *home = g_get_home_dir();
    size_t hl = strlen(home);
    if (p && strncmp(p, home, hl) == 0 && (p[hl] == '/' || p[hl] == 0)) return g_strconcat("~", p + hl, NULL);
    return g_strdup(p ? p : "");
}

gboolean path_is_dir(const char *p) { return p && g_file_test(p, G_FILE_TEST_IS_DIR); }
gboolean path_exists(const char *p) { return p && g_file_test(p, G_FILE_TEST_EXISTS); }

static char *ensure_dir(char *d) {
    g_mkdir_with_parents(d, 0700);
    return d;
}

const char *app_data_dir(void) {
    static char *d;
    if (!d) {
        const char *o = g_getenv("DSN_DATA_DIR");
        d = ensure_dir(o && *o ? g_strdup(o) : g_build_filename(g_get_user_data_dir(), "deepseek-native", NULL));
    }
    return d;
}

const char *app_config_dir(void) {
    static char *d;
    if (!d) {
        const char *o = g_getenv("DSN_DATA_DIR");
        d = ensure_dir(o && *o ? g_build_filename(o, "config", NULL) : g_build_filename(g_get_user_config_dir(), "deepseek-native", NULL));
    }
    return d;
}

const char *sessions_dir(void) {
    static char *d;
    if (!d) d = ensure_dir(g_build_filename(app_data_dir(), "sessions", NULL));
    return d;
}

const char *spill_dir(void) {
    static char *d;
    if (!d) d = ensure_dir(g_build_filename(app_data_dir(), "spill", NULL));
    return d;
}

char *spill_write(const char *text, const char *name) {
    char *u = short_uuid();
    char *fn = g_strdup_printf("%s-%s.txt", name, u);
    char *path = g_build_filename(spill_dir(), fn, NULL);
    g_free(u);
    g_free(fn);
    if (!g_file_set_contents(path, text, -1, NULL)) { g_free(path); return NULL; }
    return path;
}

/* ---------- formatting ---------- */

char *fmt_duration(double s) {
    long t = lround(s);
    if (t < 0) t = 0;
    if (t < 60) return g_strdup_printf("%lds", t);
    if (t < 3600) return g_strdup_printf("%ldm %lds", t / 60, t % 60);
    return g_strdup_printf("%ldh %ldm", t / 3600, (t % 3600) / 60);
}

char *fmt_tokens(gint64 n) {
    if (n >= 1000000) return g_strdup_printf("%.1fM", n / 1e6);
    if (n >= 100000) return g_strdup_printf("%.0fK", n / 1e3);
    if (n >= 1000) {
        char *s = g_strdup_printf("%.1fK", n / 1e3);
        char *r = str_replace(s, ".0K", "K");
        g_free(s);
        return r;
    }
    return g_strdup_printf("%" G_GINT64_FORMAT, n);
}

char *fmt_time(double t) {
    GDateTime *d = g_date_time_new_from_unix_local((gint64)t);
    char *s = g_date_time_format(d, "%H:%M");
    g_date_time_unref(d);
    return s;
}

char *fmt_relative(double t) {
    double d = now_ts() - t;
    if (d < 60) return g_strdup("now");
    if (d < 3600) return g_strdup_printf("%dmin", (int)(d / 60));
    if (d < 86400) return g_strdup_printf("%dh", (int)(d / 3600));
    if (d < 86400 * 7) return g_strdup_printf("%dd", (int)(d / 86400));
    GDateTime *dt = g_date_time_new_from_unix_local((gint64)t);
    char *s = g_date_time_format(dt, "%b %-d");
    g_date_time_unref(dt);
    return s;
}

/* ---------- JSON ---------- */

JsonNode *json_parse_str(const char *s, gssize len) {
    if (!s) return NULL;
    JsonParser *p = json_parser_new_immutable();
    JsonNode *r = NULL;
    if (json_parser_load_from_data(p, s, len, NULL)) {
        JsonNode *root = json_parser_get_root(p);
        if (root) r = json_node_copy(root);
    }
    g_object_unref(p);
    return r;
}

char *json_to_str(JsonNode *n, gboolean pretty) {
    if (!n) return g_strdup("null");
    JsonGenerator *g = json_generator_new();
    json_generator_set_pretty(g, pretty);
    if (pretty) json_generator_set_indent(g, 2);
    json_generator_set_root(g, n);
    char *s = json_generator_to_data(g, NULL);
    g_object_unref(g);
    return s;
}

char *jobj_to_str(JsonObject *o) {
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(n, o);
    char *s = json_to_str(n, FALSE);
    json_node_unref(n);
    return s;
}

static JsonNode *jmember(JsonObject *o, const char *k) {
    if (!o || !json_object_has_member(o, k)) return NULL;
    return json_object_get_member(o, k);
}

const char *jstr(JsonObject *o, const char *k) {
    JsonNode *n = jmember(o, k);
    if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE || json_node_get_value_type(n) != G_TYPE_STRING) return NULL;
    return json_node_get_string(n);
}

gint64 jint(JsonObject *o, const char *k, gint64 def) {
    JsonNode *n = jmember(o, k);
    if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE) return def;
    GType t = json_node_get_value_type(n);
    if (t == G_TYPE_INT64) return json_node_get_int(n);
    if (t == G_TYPE_DOUBLE) return (gint64)json_node_get_double(n);
    if (t == G_TYPE_BOOLEAN) return json_node_get_boolean(n);
    if (t == G_TYPE_STRING) {
        const char *s = json_node_get_string(n);
        char *end;
        gint64 v = g_ascii_strtoll(s, &end, 10);
        return end != s ? v : def;
    }
    return def;
}

double jdbl(JsonObject *o, const char *k, double def) {
    JsonNode *n = jmember(o, k);
    if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE) return def;
    GType t = json_node_get_value_type(n);
    if (t == G_TYPE_INT64) return (double)json_node_get_int(n);
    if (t == G_TYPE_DOUBLE) return json_node_get_double(n);
    if (t == G_TYPE_STRING) {
        const char *s = json_node_get_string(n);
        char *end;
        double v = g_ascii_strtod(s, &end);
        return end != s ? v : def;
    }
    return def;
}

gboolean jbool(JsonObject *o, const char *k, gboolean def) {
    JsonNode *n = jmember(o, k);
    if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE) return def;
    GType t = json_node_get_value_type(n);
    if (t == G_TYPE_BOOLEAN) return json_node_get_boolean(n);
    if (t == G_TYPE_STRING) return g_ascii_strcasecmp(json_node_get_string(n), "true") == 0;
    if (t == G_TYPE_INT64) return json_node_get_int(n) != 0;
    return def;
}

gboolean jhas(JsonObject *o, const char *k) {
    JsonNode *n = jmember(o, k);
    return n && !JSON_NODE_HOLDS_NULL(n);
}

JsonObject *jobj(JsonObject *o, const char *k) {
    JsonNode *n = jmember(o, k);
    return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
}

JsonArray *jarr(JsonObject *o, const char *k) {
    JsonNode *n = jmember(o, k);
    return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL;
}

JsonNode *jnode_str(const char *s) {
    JsonNode *n = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(n, s ? s : "");
    return n;
}

JsonNode *jnode_obj(JsonObject *o) {
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(n, o);
    return n;
}

JsonNode *jnode_arr(JsonArray *a) {
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_set_array(n, a);
    return n;
}

JsonObject *jo_new(void) { return json_object_new(); }
void jo_str(JsonObject *o, const char *k, const char *v) { json_object_set_string_member(o, k, v ? v : ""); }
void jo_int(JsonObject *o, const char *k, gint64 v) { json_object_set_int_member(o, k, v); }
void jo_dbl(JsonObject *o, const char *k, double v) { json_object_set_double_member(o, k, v); }
void jo_bool(JsonObject *o, const char *k, gboolean v) { json_object_set_boolean_member(o, k, v); }
void jo_obj(JsonObject *o, const char *k, JsonObject *v) { json_object_set_object_member(o, k, v); }
void jo_arr(JsonObject *o, const char *k, JsonArray *v) { json_object_set_array_member(o, k, v); }
JsonNode *jnode_copy(JsonNode *n) { return n ? json_node_copy(n) : NULL; }

/* ---------- threads ---------- */

static GThread *main_thread;

void main_init(void) { main_thread = g_thread_self(); }

gboolean on_main_thread(void) { return g_thread_self() == main_thread; }

typedef struct {
    void (*fn)(gpointer);
    gpointer data;
    GMutex m;
    GCond c;
    gboolean done;
} SyncCall;

static gboolean sync_cb(gpointer p) {
    SyncCall *s = p;
    s->fn(s->data);
    g_mutex_lock(&s->m);
    s->done = TRUE;
    g_cond_signal(&s->c);
    g_mutex_unlock(&s->m);
    return G_SOURCE_REMOVE;
}

void main_sync(void (*fn)(gpointer), gpointer data) {
    if (on_main_thread()) { fn(data); return; }
    SyncCall s = { .fn = fn, .data = data };
    g_mutex_init(&s.m);
    g_cond_init(&s.c);
    g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, sync_cb, &s, NULL);
    g_mutex_lock(&s.m);
    while (!s.done) g_cond_wait(&s.c, &s.m);
    g_mutex_unlock(&s.m);
    g_mutex_clear(&s.m);
    g_cond_clear(&s.c);
}

typedef struct { void (*fn)(gpointer); gpointer data; } AsyncCall;

static gboolean async_cb(gpointer p) {
    AsyncCall *a = p;
    a->fn(a->data);
    g_free(a);
    return G_SOURCE_REMOVE;
}

void main_async(void (*fn)(gpointer), gpointer data) {
    AsyncCall *a = g_new(AsyncCall, 1);
    a->fn = fn;
    a->data = data;
    g_idle_add_full(G_PRIORITY_DEFAULT, async_cb, a, NULL);
}

Waiter *waiter_new(void) {
    Waiter *w = g_new0(Waiter, 1);
    g_mutex_init(&w->m);
    g_cond_init(&w->c);
    w->refs = 1;
    return w;
}

void waiter_ref(Waiter *w) { g_atomic_int_inc(&w->refs); }

void waiter_unref(Waiter *w) {
    if (!w) return;
    if (g_atomic_int_dec_and_test(&w->refs)) {
        g_mutex_clear(&w->m);
        g_cond_clear(&w->c);
        g_free(w);
    }
}

void waiter_resolve(Waiter *w, int value, gpointer payload) {
    g_mutex_lock(&w->m);
    if (!w->done) {
        w->done = TRUE;
        w->value = value;
        w->payload = payload;
        g_cond_broadcast(&w->c);
    }
    g_mutex_unlock(&w->m);
}

gboolean waiter_wait(Waiter *w, volatile gint *cancel) {
    g_mutex_lock(&w->m);
    while (!w->done) {
        if (cancel && g_atomic_int_get(cancel)) break;
        gint64 end = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
        g_cond_wait_until(&w->c, &w->m, end);
    }
    gboolean d = w->done;
    g_mutex_unlock(&w->m);
    return d;
}

/* ---------- signatures (FNV-1a) ---------- */

Sig sig_str(Sig h, const char *s) {
    if (!s) s = "\x01";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) { h ^= *p; h *= 1099511628211ULL; }
    h ^= 0xff;
    h *= 1099511628211ULL;
    return h;
}

Sig sig_int(Sig h, gint64 v) {
    for (int i = 0; i < 8; i++) { h ^= (v >> (i * 8)) & 0xff; h *= 1099511628211ULL; }
    return h;
}

Sig sig_dbl(Sig h, double v) {
    gint64 x;
    memcpy(&x, &v, sizeof x);
    return sig_int(h, x);
}

/* ---------- files ---------- */

char *read_file(const char *path, gsize *len) {
    char *data = NULL;
    gsize l = 0;
    if (!g_file_get_contents(path, &data, &l, NULL)) return NULL;
    if (len) *len = l;
    return data;
}

gboolean write_file_atomic(const char *path, const char *data, gssize len, int mode) {
    if (len < 0) len = strlen(data);
    char *tmp = g_strdup_printf("%s.tmp-%d", path, getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode ? mode : 0644);
    if (fd < 0) { g_free(tmp); return FALSE; }
    gssize off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp);
            g_free(tmp);
            return FALSE;
        }
        off += w;
    }
    close(fd);
    gboolean ok = rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    g_free(tmp);
    return ok;
}

/* ---------- perf counters (DSN_PERF) ---------- */

static GHashTable *perf_sums;
static gint64 perf_window;

void perf_note(const char *what, gint64 usec) {
    static int enabled = -1;
    if (enabled < 0) enabled = g_getenv("DSN_PERF") != NULL;
    if (!enabled) return;
    if (!perf_sums) perf_sums = g_hash_table_new(g_str_hash, g_str_equal);
    gint64 *v = g_hash_table_lookup(perf_sums, what);
    if (!v) { v = g_new0(gint64, 2); g_hash_table_insert(perf_sums, (gpointer)what, v); }
    v[0] += usec;
    v[1]++;
    gint64 now = g_get_monotonic_time();
    if (!perf_window) perf_window = now;
    if (now - perf_window >= G_USEC_PER_SEC) {
        GHashTableIter it;
        gpointer k, val;
        GString *s = g_string_new("[perf]");
        g_hash_table_iter_init(&it, perf_sums);
        while (g_hash_table_iter_next(&it, &k, &val)) {
            gint64 *x = val;
            g_string_append_printf(s, " %s=%.1fms/%d", (char *)k, x[0] / 1000.0, (int)x[1]);
            x[0] = x[1] = 0;
        }
        fprintf(stderr, "%s\n", s->str);
        g_string_free(s, TRUE);
        perf_window = now;
    }
}
