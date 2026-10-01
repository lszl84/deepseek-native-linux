#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdint.h>

/* ---- time, ids ---- */
double now_ts(void);
char *uuid_new(void);
char *short_uuid(void);

/* ---- strings ---- */
gboolean str_blank(const char *s);
gboolean str_has_prefix(const char *s, const char *p);
gboolean str_has_suffix(const char *s, const char *p);
char *str_trim(const char *s);                 /* trims whitespace and newlines */
char *str_replace(const char *s, const char *from, const char *to);
int str_count(const char *s, const char *needle);
glong utf8_len(const char *s);
char *utf8_prefix(const char *s, glong n);     /* first n characters */
char *utf8_suffix(const char *s, glong n);     /* last n characters */
char *utf8_valid_dup(const char *s, gssize len); /* replaces invalid sequences */
char *first_line(const char *s, glong max_chars);
int count_lines(const char *s);                /* number of '\n'-separated lines */
char *ansi_strip(const char *s, gsize len);

/* ---- paths ---- */
char *path_join(const char *a, const char *b);
char *path_standardize(const char *p);
char *path_expand_tilde(const char *p);
const char *path_base(const char *p);
char *path_dirname(const char *p);
char *short_path(const char *p, const char *cwd);
char *tilde_path(const char *p);
gboolean path_is_dir(const char *p);
gboolean path_exists(const char *p);

const char *app_data_dir(void);    /* ~/.local/share/deepseek-native */
const char *app_config_dir(void);  /* ~/.config/deepseek-native */
const char *sessions_dir(void);
const char *spill_dir(void);
char *spill_write(const char *text, const char *name);

/* ---- formatting ---- */
char *fmt_duration(double s);
char *fmt_tokens(gint64 n);
char *fmt_time(double t);
char *fmt_relative(double t);

/* ---- JSON ---- */
JsonNode *json_parse_str(const char *s, gssize len);
char *json_to_str(JsonNode *n, gboolean pretty);
char *jobj_to_str(JsonObject *o);
const char *jstr(JsonObject *o, const char *k);
gint64 jint(JsonObject *o, const char *k, gint64 def);
double jdbl(JsonObject *o, const char *k, double def);
gboolean jbool(JsonObject *o, const char *k, gboolean def);
gboolean jhas(JsonObject *o, const char *k);
JsonObject *jobj(JsonObject *o, const char *k);
JsonArray *jarr(JsonObject *o, const char *k);
JsonNode *jnode_str(const char *s);
JsonNode *jnode_obj(JsonObject *o);   /* takes a reference */
JsonNode *jnode_arr(JsonArray *a);    /* takes a reference */
JsonObject *jo_new(void);
void jo_str(JsonObject *o, const char *k, const char *v);
void jo_int(JsonObject *o, const char *k, gint64 v);
void jo_dbl(JsonObject *o, const char *k, double v);
void jo_bool(JsonObject *o, const char *k, gboolean v);
void jo_obj(JsonObject *o, const char *k, JsonObject *v); /* takes ownership */
void jo_arr(JsonObject *o, const char *k, JsonArray *v);  /* takes ownership */
JsonNode *jnode_copy(JsonNode *n);

/* ---- threads ---- */
gboolean on_main_thread(void);
void main_init(void);
/* Runs fn(data) on the main thread and waits for it. Runs inline when already on main. */
void main_sync(void (*fn)(gpointer), gpointer data);
/* Queues fn(data) on the main thread. */
void main_async(void (*fn)(gpointer), gpointer data);

/* Generic one-shot waiter used to block a worker until the UI resolves something. */
typedef struct {
    GMutex m;
    GCond c;
    gboolean done;
    int value;
    gpointer payload;
    int refs;
} Waiter;
Waiter *waiter_new(void);
void waiter_ref(Waiter *w);
void waiter_unref(Waiter *w);
void waiter_resolve(Waiter *w, int value, gpointer payload);
/* Waits until resolved; checks *cancel every 100ms. Returns TRUE when resolved. */
gboolean waiter_wait(Waiter *w, volatile gint *cancel);

/* ---- signatures ---- */
typedef guint64 Sig;
static inline Sig sig_init(void) { return 1469598103934665603ULL; }
Sig sig_str(Sig h, const char *s);
Sig sig_int(Sig h, gint64 v);
Sig sig_dbl(Sig h, double v);

/* ---- debug ---- */
extern gboolean debug_logging;
void dlog(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

/* ---- base64 / misc ---- */
char *read_file(const char *path, gsize *len);
gboolean write_file_atomic(const char *path, const char *data, gssize len, int mode);

/* Accumulates timings and prints per-second totals to stderr when DSN_PERF is set. */
void perf_note(const char *what, gint64 usec);
