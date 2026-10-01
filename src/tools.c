#include "agent.h"
#include "presentation.h"
#include "store.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <curl/curl.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* ======================= helpers ======================= */

static JsonObject *meta_new(void) { return jo_new(); }

/* Returns an error result when the permission mode forbids writing `path` and the user declines. */
static ToolResult *check_write(const char *path, ToolCtx *ctx, const char *verb) {
    PermissionMode mode = ctx_permission(ctx);
    if (mode == PERM_FULL_ACCESS) return NULL;
    if (mode == PERM_WORKSPACE_WRITE && ctx_inside_workspace(ctx, path)) return NULL;
    const char *where = mode == PERM_READ_ONLY ? "in read-only mode" : "outside the workspace";
    char *title = g_strdup_printf("%s %s %s?", verb, path_base(path), where);
    char *key = g_strconcat("write:", path, NULL);
    gboolean ok = agent_request_approval(ctx->agent, ctx->call_id, title, path, key);
    g_free(title);
    g_free(key);
    if (ok) return NULL;
    char *lv = g_ascii_strdown(verb, -1);
    ToolResult *r = tool_errf("The user denied %s %s. Do not retry another way; ask the user how to proceed if it is required.", lv, path);
    g_free(lv);
    return r;
}

/* Writes text to path; existing files are rewritten in place so mode, owner and links survive. */
static gboolean write_text(const char *path, const char *text, gsize len, char **err) {
    struct stat st;
    gboolean exists = stat(path, &st) == 0;
    if (!exists) {
        char *dir = path_dirname(path);
        if (g_mkdir_with_parents(dir, 0755) != 0) {
            *err = g_strdup_printf("cannot create directory %s: %s", dir, g_strerror(errno));
            g_free(dir);
            return FALSE;
        }
        g_free(dir);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { *err = g_strdup(g_strerror(errno)); return FALSE; }
    gsize off = 0;
    while (off < len) {
        ssize_t w = write(fd, text + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            *err = g_strdup(g_strerror(errno));
            close(fd);
            return FALSE;
        }
        off += w;
    }
    if (close(fd) != 0) { *err = g_strdup(g_strerror(errno)); return FALSE; }
    return TRUE;
}

static int line_count(const char *s) {
    if (!s || !*s) return 0;
    return count_lines(s);
}

/* ======================= bash ======================= */

static ToolResult *run_bash(JsonObject *in, ToolCtx *ctx) {
    const char *command = jstr(in, "command");
    if (str_blank(command)) return tool_err("command is required");
    char *workdir = ctx_resolve(ctx, jstr(in, "workdir"));
    if (!path_is_dir(workdir)) {
        ToolResult *r = tool_errf("workdir does not exist: %s", workdir);
        g_free(workdir);
        return r;
    }
    PermissionMode mode = ctx_permission(ctx);
    int sb = sandbox_ruleset(mode, ctx->cwd);
    if (sb == -2) {
        char *key = g_strconcat("bash:", command, NULL);
        gboolean ok = agent_request_approval(ctx->agent, ctx->call_id, "Run command without sandbox?", command, key);
        g_free(key);
        if (!ok) { g_free(workdir); return tool_err("The user denied this command."); }
        sb = -1;
    }
    char *argv[] = { "/bin/bash", "-c", (char *)command, NULL };
    char **env = shell_environment(workdir, ctx->agent->session->id);
    GError *err = NULL;
    ChildProcess *p = proc_spawn(argv, workdir, env, sb, &err);
    g_strfreev(env);
    if (sb >= 0) close(sb);
    if (!p) {
        ToolResult *r = tool_errf("Failed to start command: %s", err ? err->message : "unknown error");
        g_clear_error(&err);
        g_free(workdir);
        return r;
    }
    g_free(workdir);
    const char *desc = jstr(in, "description");
    char *label = desc && *desc ? g_strdup(desc) : utf8_prefix(command, 80);
    volatile gint *cancel = ctx_cancel(ctx);
    if (jbool(in, "run_in_background", FALSE)) {
        const char *id = jobs_adopt(ctx->agent->jobs, p, label);
        JsonObject *m = meta_new();
        jo_str(m, "job", id);
        ToolResult *r = tool_ok(NULL);
        g_free(r->text);
        r->text = g_strdup_printf("Started background job %s. Collect output with job_output, stop with job_kill.", id);
        r->meta = m;
        proc_unref(p);
        g_free(label);
        return r;
    }
    double requested = jhas(in, "timeoutMs") ? jdbl(in, "timeoutMs", 0) / 1000.0 : settings()->bash_timeout_ms / 1000.0;
    double timeout = CLAMP(requested, 1, 600);
    gint64 deadline = g_get_monotonic_time() + (gint64)(timeout * G_USEC_PER_SEC);
    gboolean done = FALSE;
    char *last_progress = NULL;
    while (!done) {
        if (g_atomic_int_get(cancel)) break;
        gint64 now = g_get_monotonic_time();
        if (now >= deadline) break;
        double slice = MIN(0.25, (deadline - now) / (double)G_USEC_PER_SEC);
        done = proc_wait(p, slice, cancel);
        if (!done) {
            char *out = proc_output(p, 4000, NULL, NULL);
            if (*out && g_strcmp0(out, last_progress)) {
                ctx_progress(ctx, out);
                g_free(last_progress);
                last_progress = out;
            } else g_free(out);
        }
    }
    g_free(last_progress);
    ToolResult *r;
    if (!done && g_atomic_int_get(cancel)) {
        proc_terminate(p);
        char *out = proc_output(p, 30000, NULL, NULL);
        r = tool_errf("%s\n[interrupted by the user]", out);
        g_free(out);
    } else if (!done) {
        const char *id = jobs_adopt(ctx->agent->jobs, p, label);
        char *out = proc_output(p, 8000, NULL, NULL);
        r = tool_ok(NULL);
        g_free(r->text);
        r->text = g_strdup_printf("Command still running after %ds; it moved to the background as job %s. Collect with job_output, stop with job_kill.\nOutput so far:\n%s",
                                  (int)timeout, id, out);
        g_free(out);
        JsonObject *m = meta_new();
        jo_str(m, "job", id);
        r->meta = m;
        JsonObject *m2 = meta_new();
        jo_str(m2, "job", id);
        ctx_set_meta(ctx, m2);
    } else {
        int code = proc_exit_code(p);
        char *spill = NULL;
        char *out = proc_output(p, 30000, NULL, &spill);
        size_t ol = strlen(out);
        while (ol && out[ol - 1] == '\n') out[--ol] = 0;
        GString *t = g_string_new(*out ? out : "(no output)");
        if (spill) g_string_append_printf(t, "\n[full output saved to %s]", spill);
        g_string_append_printf(t, "\n[exit code: %d]", code);
        r = tool_ok(t->str);
        g_string_free(t, TRUE);
        g_free(out);
        g_free(spill);
        JsonObject *m = meta_new();
        jo_int(m, "exitCode", code);
        r->meta = m;
    }
    proc_unref(p);
    g_free(label);
    return r;
}

/* ======================= read ======================= */

static gint cmp_str(gconstpointer a, gconstpointer b) { return strcmp(*(char **)a, *(char **)b); }

static ToolResult *run_read(JsonObject *in, ToolCtx *ctx) {
    char *path = ctx_resolve(ctx, jstr(in, "file_path"));
    ToolResult *r = NULL;
    if (!path_exists(path)) {
        ctx_observe(ctx, path);
        r = tool_errf("File not found: %s", path);
        g_free(path);
        return r;
    }
    if (path_is_dir(path)) {
        GDir *d = g_dir_open(path, 0, NULL);
        GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
        const char *n;
        while (d && (n = g_dir_read_name(d))) {
            char *full = g_build_filename(path, n, NULL);
            g_ptr_array_add(items, path_is_dir(full) ? g_strconcat(n, "/", NULL) : g_strdup(n));
            g_free(full);
        }
        if (d) g_dir_close(d);
        g_ptr_array_sort(items, cmp_str);
        GString *s = g_string_new("");
        g_string_append_printf(s, "%s is a directory. Entries:", path);
        for (guint i = 0; i < items->len && i < 500; i++) g_string_append_printf(s, "\n%s", (char *)g_ptr_array_index(items, i));
        r = tool_err(s->str);
        g_string_free(s, TRUE);
        g_ptr_array_unref(items);
        g_free(path);
        return r;
    }
    gsize len = 0;
    char *data = read_file(path, &len);
    if (!data) {
        r = tool_errf("Cannot read %s", path);
        g_free(path);
        return r;
    }
    if (memchr(data, 0, MIN(len, 8192))) {
        r = tool_errf("%s is a binary file (%zu bytes); it cannot be read as text.", path, len);
        g_free(data);
        g_free(path);
        return r;
    }
    char *text = utf8_valid_dup(data, len);
    g_free(data);
    ctx_observe(ctx, path);
    gint64 offset = MAX(1, jint(in, "offset", 1));
    gint64 limit = MAX(1, jint(in, "limit", 2000));
    /* split into lines without copying */
    GPtrArray *lines = g_ptr_array_new();
    char *p = text;
    while (*p) {
        g_ptr_array_add(lines, p);
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = 0;
        p = nl + 1;
    }
    gint64 total = lines->len;
    if (total == 0) {
        r = tool_ok("(empty file)");
        r->meta = meta_new();
        jo_int(r->meta, "lines", 0);
    } else if (offset > total) {
        r = tool_errf("offset %" G_GINT64_FORMAT " is past the end of the file (%" G_GINT64_FORMAT " lines).", offset, total);
    } else {
        gint64 end = MIN(total, offset - 1 + limit);
        GString *out = g_string_sized_new(MIN(len, 400000));
        gint64 last = offset - 1;
        gsize chars = 0;
        for (gint64 i = offset - 1; i < end; i++) {
            const char *l = g_ptr_array_index(lines, i);
            char *ll = NULL;
            if (g_utf8_strlen(l, -1) > 2000) {
                char *pre = utf8_prefix(l, 2000);
                ll = g_strconcat(pre, " … [line truncated]", NULL);
                g_free(pre);
            }
            char *row = g_strdup_printf("%6" G_GINT64_FORMAT "\t%s\n", i + 1, ll ? ll : l);
            g_free(ll);
            chars += strlen(row);
            if (chars > 250000) { g_free(row); break; }
            g_string_append(out, row);
            g_free(row);
            last = i + 1;
        }
        if (last < total)
            g_string_append_printf(out, "\n[Showing lines %" G_GINT64_FORMAT "-%" G_GINT64_FORMAT " of %" G_GINT64_FORMAT ". Use offset %" G_GINT64_FORMAT " to continue.]",
                                   offset, last, total, last + 1);
        r = tool_ok(out->str);
        g_string_free(out, TRUE);
        r->meta = meta_new();
        jo_int(r->meta, "lines", last - offset + 1);
        jo_int(r->meta, "total", total);
    }
    g_ptr_array_unref(lines);
    g_free(text);
    g_free(path);
    return r;
}

/* ======================= write ======================= */

static ToolResult *run_write(JsonObject *in, ToolCtx *ctx) {
    char *path = ctx_resolve(ctx, jstr(in, "file_path"));
    const char *content = jstr(in, "content");
    ToolResult *r = NULL;
    if (!content) { g_free(path); return tool_err("content is required"); }
    gboolean exists = path_exists(path);
    if (exists && path_is_dir(path)) { r = tool_errf("%s is a directory.", path); g_free(path); return r; }
    if (exists && !ctx_has_observed(ctx, path)) {
        r = tool_errf("%s already exists and has not been read in this session. Read it before overwriting it, or use edit for targeted changes.", path);
        g_free(path);
        return r;
    }
    if ((r = check_write(path, ctx, exists ? "Overwrite" : "Create"))) { g_free(path); return r; }
    char *old = exists ? read_file(path, NULL) : NULL;
    char *err = NULL;
    if (!write_text(path, content, strlen(content), &err)) {
        r = tool_errf("Failed to write %s: %s", path, err);
        g_free(err);
        g_free(old);
        g_free(path);
        return r;
    }
    ctx_observe(ctx, path);
    int lines = line_count(content);
    int old_lines = old ? count_lines(old) : 0;
    r = tool_ok(NULL);
    g_free(r->text);
    r->text = g_strdup_printf("%s %s (%d lines).", exists ? "Overwrote" : "Created", path, lines);
    r->meta = meta_new();
    jo_int(r->meta, "added", lines);
    jo_int(r->meta, "removed", old_lines);
    jo_bool(r->meta, "created", !exists);
    g_free(old);
    g_free(path);
    return r;
}

/* ======================= edit ======================= */

static ToolResult *run_edit(JsonObject *in, ToolCtx *ctx) {
    char *path = ctx_resolve(ctx, jstr(in, "file_path"));
    const char *old = jstr(in, "old_string"), *nw = jstr(in, "new_string");
    ToolResult *r = NULL;
    if (!old || !nw) { r = tool_err("old_string and new_string are required"); goto out; }
    if (!path_exists(path)) { r = tool_errf("File not found: %s. Use write to create a new file.", path); goto out; }
    if (!ctx_has_observed(ctx, path)) { r = tool_errf("%s has not been read in this session. Read it before editing.", path); goto out; }
    if (!*old) { r = tool_err("old_string must not be empty."); goto out; }
    if (!strcmp(old, nw)) { r = tool_err("old_string and new_string are identical."); goto out; }
    gsize len;
    char *text = read_file(path, &len);
    if (!text || !g_utf8_validate(text, len, NULL)) { g_free(text); r = tool_errf("Cannot read %s as UTF-8.", path); goto out; }
    int count = str_count(text, old);
    gboolean all = jbool(in, "replace_all", FALSE);
    if (count == 0) {
        g_free(text);
        r = tool_errf("old_string was not found in %s. Read the file again to get the exact current text, including whitespace.", path);
        goto out;
    }
    if (count > 1 && !all) {
        g_free(text);
        r = tool_errf("old_string appears %d times in %s. Include more surrounding context to make it unique, or set replace_all.", count, path);
        goto out;
    }
    if ((r = check_write(path, ctx, "Edit"))) { g_free(text); goto out; }
    char *first = strstr(text, old);
    int start_line = 1;
    for (char *q = text; q < first; q++)
        if (*q == '\n') start_line++;
    char *updated;
    if (all) updated = str_replace(text, old, nw);
    else {
        GString *u = g_string_new_len(text, first - text);
        g_string_append(u, nw);
        g_string_append(u, first + strlen(old));
        updated = g_string_free(u, FALSE);
    }
    char *err = NULL;
    if (!write_text(path, updated, strlen(updated), &err)) {
        r = tool_errf("Failed to write %s: %s", path, err);
        g_free(err);
    } else {
        ctx_observe(ctx, path);
        int n = all ? count : 1;
        r = tool_ok(NULL);
        g_free(r->text);
        r->text = g_strdup_printf("Edited %s: replaced %d occurrence%s.", path, n, n > 1 ? "s" : "");
        r->meta = meta_new();
        jo_int(r->meta, "added", *nw ? count_lines(nw) * n : 0);
        jo_int(r->meta, "removed", count_lines(old) * n);
        jo_int(r->meta, "line", start_line);
    }
    g_free(updated);
    g_free(text);
out:
    g_free(path);
    return r;
}

/* ======================= read_image ======================= */

ImageData *image_load_scaled(const char *path, int max_side, int *out_w, int *out_h) {
    int w = 0, h = 0;
    GdkPixbufFormat *fmt = gdk_pixbuf_get_file_info(path, &w, &h);
    if (!fmt) return NULL;
    GError *err = NULL;
    GdkPixbuf *pb = (w > max_side || h > max_side) ? gdk_pixbuf_new_from_file_at_scale(path, max_side, max_side, TRUE, &err)
                                                   : gdk_pixbuf_new_from_file(path, &err);
    if (!pb) { g_clear_error(&err); return NULL; }
    GdkPixbuf *oriented = gdk_pixbuf_apply_embedded_orientation(pb);
    g_object_unref(pb);
    pb = oriented;
    gboolean alpha = gdk_pixbuf_get_has_alpha(pb);
    gchar *buf = NULL;
    gsize size = 0;
    gboolean ok = alpha ? gdk_pixbuf_save_to_buffer(pb, &buf, &size, "png", &err, NULL)
                        : gdk_pixbuf_save_to_buffer(pb, &buf, &size, "jpeg", &err, "quality", "85", NULL);
    if (out_w) *out_w = gdk_pixbuf_get_width(pb);
    if (out_h) *out_h = gdk_pixbuf_get_height(pb);
    g_object_unref(pb);
    if (!ok) { g_clear_error(&err); return NULL; }
    char *b64 = g_base64_encode((const guchar *)buf, size);
    g_free(buf);
    ImageData *d = image_data_new(alpha ? "image/png" : "image/jpeg", b64, path);
    g_free(b64);
    return d;
}

static ToolResult *run_read_image(JsonObject *in, ToolCtx *ctx) {
    char *path = ctx_resolve(ctx, jstr(in, "file_path"));
    int w = 0, h = 0;
    ImageData *img = image_load_scaled(path, 1568, &w, &h);
    ToolResult *r;
    if (!img) r = tool_errf("Cannot decode image at %s", path);
    else {
        r = tool_ok(NULL);
        g_free(r->text);
        r->text = g_strdup_printf("Image %s (%d×%d):", path, w, h);
        r->images = g_ptr_array_new_with_free_func(image_data_free);
        g_ptr_array_add(r->images, img);
        r->meta = meta_new();
        jo_int(r->meta, "width", w);
        jo_int(r->meta, "height", h);
    }
    g_free(path);
    return r;
}

/* ======================= glob / grep ======================= */

static char *glob_to_regex(const char *g) {
    GString *r = g_string_new("^");
    for (const char *p = g; *p; p++) {
        char c = *p;
        if (c == '*') {
            if (p[1] == '*') {
                if (p[2] == '/') { g_string_append(r, "(?:.*/)?"); p += 2; continue; }
                g_string_append(r, ".*");
                p++;
                continue;
            }
            g_string_append(r, "[^/]*");
        } else if (c == '?') g_string_append(r, "[^/]");
        else if (c == '{') g_string_append(r, "(?:");
        else if (c == '}') g_string_append(r, ")");
        else if (c == ',') g_string_append(r, "|");
        else {
            char s[2] = { c, 0 };
            char *e = g_regex_escape_string(s, 1);
            g_string_append(r, e);
            g_free(e);
        }
    }
    g_string_append_c(r, '$');
    return g_string_free(r, FALSE);
}

static gboolean skip_dir(const char *n) {
    return !strcmp(n, ".git") || !strcmp(n, "node_modules") || !strcmp(n, ".build") || !strcmp(n, "target") || !strcmp(n, "__pycache__");
}

typedef gboolean (*WalkFn)(const char *rel, gpointer data);

static gboolean walk_rec(const char *root, const char *rel, WalkFn fn, gpointer data, int depth) {
    if (depth > 40) return TRUE;
    char *dir = rel ? g_build_filename(root, rel, NULL) : g_strdup(root);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) { g_free(dir); return TRUE; }
    const char *n;
    gboolean cont = TRUE;
    while (cont && (n = g_dir_read_name(d))) {
        char *r = rel ? g_build_filename(rel, n, NULL) : g_strdup(n);
        char *full = g_build_filename(dir, n, NULL);
        struct stat st;
        if (lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (!skip_dir(n)) cont = walk_rec(root, r, fn, data, depth + 1);
            } else if (S_ISREG(st.st_mode)) {
                cont = fn(r, data);
            }
        }
        g_free(full);
        g_free(r);
    }
    g_dir_close(d);
    g_free(dir);
    return cont;
}

typedef struct { GRegex *rx; GPtrArray *out; } GlobWalk;

static gboolean glob_visit(const char *rel, gpointer data) {
    GlobWalk *g = data;
    if (g_regex_match(g->rx, rel, 0, NULL)) g_ptr_array_add(g->out, g_strdup(rel));
    return g->out->len < 50000;
}

typedef struct { char *path; gint64 mtime; } TimedPath;

static gint cmp_time(gconstpointer a, gconstpointer b) {
    const TimedPath *x = a, *y = b;
    return x->mtime > y->mtime ? -1 : x->mtime < y->mtime ? 1 : strcmp(x->path, y->path);
}

static ToolResult *run_glob(JsonObject *in, ToolCtx *ctx) {
    const char *pattern = jstr(in, "pattern");
    if (str_blank(pattern)) return tool_err("pattern is required");
    char *root = ctx_resolve(ctx, jstr(in, "path"));
    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    const char *rg = ripgrep_path();
    if (rg) {
        char *argv[] = { (char *)rg, "--files", "--hidden", "--no-ignore", "--no-messages", "-g", (char *)pattern, "-g", "!.git/", NULL };
        int code = 0;
        char *out = proc_run_capture(argv, root, 60, &code, ctx_cancel(ctx));
        if (code > 1) {
            ToolResult *r = tool_err(*out ? out : "glob failed");
            g_free(out);
            g_free(root);
            g_ptr_array_unref(files);
            return r;
        }
        char **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i]; i++)
            if (*lines[i]) g_ptr_array_add(files, g_strdup(str_has_prefix(lines[i], "./") ? lines[i] + 2 : lines[i]));
        g_strfreev(lines);
        g_free(out);
    } else {
        char *anch = strchr(pattern, '/') ? g_strdup(pattern) : g_strconcat("**/", pattern, NULL);
        char *rxs = glob_to_regex(anch);
        g_free(anch);
        GRegex *rx = g_regex_new(rxs, 0, 0, NULL);
        g_free(rxs);
        if (rx) {
            GlobWalk gw = { rx, files };
            walk_rec(root, NULL, glob_visit, &gw, 0);
            g_regex_unref(rx);
        }
    }
    ToolResult *r;
    if (!files->len) {
        r = tool_ok(NULL);
        g_free(r->text);
        r->text = g_strdup_printf("No files found matching %s in %s.", pattern, root);
        r->meta = meta_new();
        jo_int(r->meta, "count", 0);
    } else {
        guint n = MIN(files->len, 20000);
        GArray *tp = g_array_sized_new(FALSE, FALSE, sizeof(TimedPath), n);
        for (guint i = 0; i < n; i++) {
            TimedPath t = { g_ptr_array_index(files, i), 0 };
            char *full = g_build_filename(root, t.path, NULL);
            struct stat st;
            if (stat(full, &st) == 0) t.mtime = (gint64)st.st_mtime;
            g_free(full);
            g_array_append_val(tp, t);
        }
        g_array_sort(tp, cmp_time);
        GString *text = g_string_new("");
        if (tp->len > 100) {
            /* Sample across top-level entries so one huge directory does not hide the rest. */
            GHashTable *buckets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
            GPtrArray *order = g_ptr_array_new();
            for (guint i = 0; i < tp->len; i++) {
                const char *f = g_array_index(tp, TimedPath, i).path;
                const char *sl = strchr(f, '/');
                char *top = sl ? g_strndup(f, sl - f) : g_strdup(f);
                GPtrArray *b = g_hash_table_lookup(buckets, top);
                if (!b) {
                    b = g_ptr_array_new();
                    g_hash_table_insert(buckets, top, b);
                    g_ptr_array_add(order, b);
                } else g_free(top);
                g_ptr_array_add(b, (gpointer)f);
            }
            guint picked = 0;
            for (guint round = 0; picked < 100; round++) {
                gboolean added = FALSE;
                for (guint k = 0; k < order->len && picked < 100; k++) {
                    GPtrArray *b = g_ptr_array_index(order, k);
                    if (round < b->len) {
                        g_string_append_printf(text, "%s%s", picked ? "\n" : "", (char *)g_ptr_array_index(b, round));
                        picked++;
                        added = TRUE;
                    }
                }
                if (!added) break;
            }
            GString *all = g_string_new("");
            for (guint i = 0; i < tp->len; i++) g_string_append_printf(all, "%s\n", g_array_index(tp, TimedPath, i).path);
            char *saved = spill_write(all->str, "glob");
            g_string_free(all, TRUE);
            g_string_append_printf(text, "\n\n[%u files matched; showing a sample of 100.", files->len);
            if (saved) g_string_append_printf(text, " Complete list saved to %s", saved);
            g_string_append(text, "]");
            g_free(saved);
            g_ptr_array_free(order, TRUE);
            g_hash_table_unref(buckets);
        } else {
            for (guint i = 0; i < tp->len; i++) g_string_append_printf(text, "%s%s", i ? "\n" : "", g_array_index(tp, TimedPath, i).path);
        }
        g_array_unref(tp);
        r = tool_ok(text->str);
        g_string_free(text, TRUE);
        r->meta = meta_new();
        jo_int(r->meta, "count", files->len);
    }
    g_ptr_array_unref(files);
    g_free(root);
    return r;
}

typedef struct { GRegex *rx; GRegex *inc; const char *root; GString *out; int count; } GrepWalk;

static gboolean grep_visit(const char *rel, gpointer data) {
    GrepWalk *g = data;
    if (g->inc && !g_regex_match(g->inc, rel, 0, NULL)) return TRUE;
    char *full = g_build_filename(g->root, rel, NULL);
    gsize len;
    char *d = read_file(full, &len);
    if (!d || len > 5000000 || memchr(d, 0, MIN(len, 4096))) { g_free(d); g_free(full); return TRUE; }
    char *text = utf8_valid_dup(d, len);
    g_free(d);
    gboolean header = FALSE;
    int n = 0;
    char *p = text;
    while (p && *p) {
        n++;
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        if (g_regex_match(g->rx, p, 0, NULL)) {
            if (!header) { g_string_append_printf(g->out, "%s%s\n", g->out->len ? "\n" : "", full); header = TRUE; }
            char *pre = utf8_prefix(p, 400);
            g_string_append_printf(g->out, "%d:%s\n", n, pre);
            g_free(pre);
            g->count++;
        }
        p = nl ? nl + 1 : NULL;
    }
    g_free(text);
    g_free(full);
    return g->count < 5000;
}

static ToolResult *run_grep(JsonObject *in, ToolCtx *ctx) {
    const char *pattern = jstr(in, "pattern");
    if (str_blank(pattern)) return tool_err("pattern is required");
    char *target = ctx_resolve(ctx, jstr(in, "path"));
    const char *include = jstr(in, "include");
    char *out = NULL;
    ToolResult *r = NULL;
    const char *rg = ripgrep_path();
    if (rg) {
        GPtrArray *argv = g_ptr_array_new();
        const char *base[] = { rg, "--heading", "-n", "--color", "never", "--max-columns", "400", "--max-columns-preview", "--no-messages", "--hidden", "-g", "!.git/" };
        for (size_t i = 0; i < G_N_ELEMENTS(base); i++) g_ptr_array_add(argv, (gpointer)base[i]);
        if (include && *include) { g_ptr_array_add(argv, "-g"); g_ptr_array_add(argv, (gpointer)include); }
        g_ptr_array_add(argv, "-e");
        g_ptr_array_add(argv, (gpointer)pattern);
        g_ptr_array_add(argv, target);
        g_ptr_array_add(argv, NULL);
        int code = 0;
        out = proc_run_capture((char **)argv->pdata, ctx->cwd, 60, &code, ctx_cancel(ctx));
        g_ptr_array_free(argv, TRUE);
        if (code == 1) {
            r = tool_ok(NULL);
            g_free(r->text);
            r->text = g_strdup_printf("No matches for %s.", pattern);
            r->meta = meta_new();
            jo_int(r->meta, "count", 0);
            goto done;
        }
        if (code > 1 || code < 0) { r = *out ? tool_err(out) : tool_errf("grep failed (exit %d)", code); goto done; }
    } else {
        GError *err = NULL;
        GRegex *rx = g_regex_new(pattern, G_REGEX_OPTIMIZE, 0, &err);
        if (!rx) { r = tool_errf("Invalid regular expression: %s", err->message); g_error_free(err); goto done; }
        GRegex *inc = NULL;
        if (include && *include) {
            char *g = g_strconcat("**/", include, NULL);
            char *rxs = glob_to_regex(g);
            inc = g_regex_new(rxs, 0, 0, NULL);
            g_free(rxs);
            g_free(g);
        }
        GrepWalk gw = { rx, inc, target, g_string_new(""), 0 };
        if (path_is_dir(target)) walk_rec(target, NULL, grep_visit, &gw, 0);
        else {
            char *dir = path_dirname(target);
            gw.root = dir;
            grep_visit(path_base(target), &gw);
            g_free(dir);
        }
        g_regex_unref(rx);
        if (inc) g_regex_unref(inc);
        out = g_string_free(gw.out, FALSE);
        if (!*out) {
            r = tool_ok(NULL);
            g_free(r->text);
            r->text = g_strdup_printf("No matches for %s.", pattern);
            r->meta = meta_new();
            jo_int(r->meta, "count", 0);
            goto done;
        }
    }
    {
        /* Make file headings relative to the working directory. */
        char *prefix = g_str_has_suffix(ctx->cwd, "/") ? g_strdup(ctx->cwd) : g_strconcat(ctx->cwd, "/", NULL);
        size_t pl = strlen(prefix);
        char **lines = g_strsplit(out, "\n", -1);
        GString *text = g_string_new("");
        int matches = 0, total = 0;
        gboolean cut = FALSE;
        for (int i = 0; lines[i]; i++) {
            const char *l = lines[i];
            gboolean is_match = g_ascii_isdigit(l[0]) && strchr(l, ':');
            if (is_match) total++;
            if (cut) continue;
            if (is_match && matches >= 250) { cut = TRUE; continue; }
            if (is_match) matches++;
            if (text->len) g_string_append_c(text, '\n');
            g_string_append(text, strncmp(l, prefix, pl) == 0 ? l + pl : l);
        }
        g_strfreev(lines);
        g_free(prefix);
        while (text->len && text->str[text->len - 1] == '\n') g_string_truncate(text, text->len - 1);
        if (cut) {
            char *saved = spill_write(out, "grep");
            g_string_append_printf(text, "\n\n[%d matches; showing the first 250.", total);
            if (saved) g_string_append_printf(text, " Complete results saved to %s", saved);
            g_string_append(text, "]");
            g_free(saved);
        }
        r = tool_ok(text->str);
        g_string_free(text, TRUE);
        r->meta = meta_new();
        jo_int(r->meta, "count", total);
    }
done:
    g_free(out);
    g_free(target);
    return r;
}

/* ======================= todo_write ======================= */

typedef struct { Session *s; GPtrArray *todos; int added; } TodoCtx;

static void todo_main(gpointer p) {
    TodoCtx *c = p;
    int added = 0;
    for (guint i = 0; i < c->todos->len; i++) {
        TodoItem *t = g_ptr_array_index(c->todos, i);
        gboolean found = FALSE;
        for (guint j = 0; j < c->s->todos->len && !found; j++)
            if (!g_strcmp0(((TodoItem *)g_ptr_array_index(c->s->todos, j))->content, t->content)) found = TRUE;
        if (!found) added++;
    }
    c->added = added;
    session_set_todos(c->s, c->todos);
}

static ToolResult *run_todo_write(JsonObject *in, ToolCtx *ctx) {
    JsonArray *a = jarr(in, "todos");
    if (!a) return tool_err("todos must be an array");
    GPtrArray *todos = g_ptr_array_new_with_free_func(todo_free);
    int done = 0;
    for (guint i = 0; i < json_array_get_length(a); i++) {
        JsonNode *n = json_array_get_element(a, i);
        if (!JSON_NODE_HOLDS_OBJECT(n)) continue;
        JsonObject *t = json_node_get_object(n);
        if (!jstr(t, "content")) continue;
        const char *st = jstr(t, "status");
        if (!st || (strcmp(st, "pending") && strcmp(st, "in_progress") && strcmp(st, "completed"))) st = "pending";
        TodoItem *ti = g_new0(TodoItem, 1);
        ti->content = g_strdup(jstr(t, "content"));
        ti->status = g_strdup(st);
        if (!strcmp(st, "completed")) done++;
        g_ptr_array_add(todos, ti);
    }
    int total = todos->len;
    TodoCtx c = { ctx->agent->session, todos, 0 };
    main_sync(todo_main, &c);
    ToolResult *r = tool_ok(NULL);
    g_free(r->text);
    r->text = g_strdup_printf("Todo list updated: %d/%d completed.", done, total);
    r->meta = meta_new();
    jo_int(r->meta, "done", done);
    jo_int(r->meta, "total", total);
    jo_int(r->meta, "added", c.added);
    return r;
}

/* ======================= ask_user_question ======================= */

static ToolResult *run_ask(JsonObject *in, ToolCtx *ctx) {
    JsonArray *a = jarr(in, "questions");
    GPtrArray *qs = g_ptr_array_new_with_free_func(question_free);
    for (guint i = 0; a && i < json_array_get_length(a); i++) {
        JsonNode *n = json_array_get_element(a, i);
        if (!JSON_NODE_HOLDS_OBJECT(n)) continue;
        JsonObject *q = json_node_get_object(n);
        if (!jstr(q, "question")) continue;
        Question *x = g_new0(Question, 1);
        x->id = jstr(q, "id") ? g_strdup(jstr(q, "id")) : g_strdup_printf("q%u", i + 1);
        x->header = g_strdup(jstr(q, "header"));
        x->question = g_strdup(jstr(q, "question"));
        x->labels = g_ptr_array_new_with_free_func(g_free);
        x->descs = g_ptr_array_new_with_free_func(g_free);
        JsonArray *opts = jarr(q, "options");
        for (guint j = 0; opts && j < json_array_get_length(opts); j++) {
            JsonNode *on = json_array_get_element(opts, j);
            if (!JSON_NODE_HOLDS_OBJECT(on)) continue;
            JsonObject *o = json_node_get_object(on);
            if (!jstr(o, "label")) continue;
            g_ptr_array_add(x->labels, g_strdup(jstr(o, "label")));
            g_ptr_array_add(x->descs, g_strdup(jstr(o, "description") ? jstr(o, "description") : ""));
        }
        x->multi = jbool(q, "multi_select", FALSE);
        g_ptr_array_add(qs, x);
    }
    if (!qs->len) { g_ptr_array_unref(qs); return tool_err("questions must contain at least one question"); }
    /* keep ids and texts for the result; the request takes ownership of qs */
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free), *texts = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < qs->len; i++) {
        Question *q = g_ptr_array_index(qs, i);
        g_ptr_array_add(ids, g_strdup(q->id));
        g_ptr_array_add(texts, g_strdup(q->question));
    }
    JsonObject *answers = agent_ask_user(ctx->agent, ctx->call_id, qs);
    ToolResult *r;
    if (!answers) r = tool_err("The user dismissed the question without answering.");
    else {
        GString *s = g_string_new("");
        JsonObject *meta = meta_new();
        JsonObject *am = jo_new();
        for (guint i = 0; i < ids->len; i++) {
            const char *id = g_ptr_array_index(ids, i);
            JsonArray *ans = jarr(answers, id);
            GString *joined = g_string_new("");
            JsonArray *copy = json_array_new();
            for (guint j = 0; ans && j < json_array_get_length(ans); j++) {
                const char *v = json_node_get_string(json_array_get_element(ans, j));
                if (!v) continue;
                if (joined->len) g_string_append(joined, ", ");
                g_string_append(joined, v);
                json_array_add_string_element(copy, v);
            }
            g_string_append_printf(s, "%s%s: %s\nAnswer: %s", i ? "\n\n" : "", id, (char *)g_ptr_array_index(texts, i), joined->len ? joined->str : "(no answer)");
            jo_arr(am, id, copy);
            g_string_free(joined, TRUE);
        }
        jo_obj(meta, "answers", am);
        r = tool_ok(s->str);
        r->meta = meta;
        g_string_free(s, TRUE);
        json_object_unref(answers);
    }
    g_ptr_array_unref(ids);
    g_ptr_array_unref(texts);
    return r;
}

/* ======================= present ======================= */

static ToolResult *run_present(JsonObject *in, ToolCtx *ctx) {
    JsonArray *a = jarr(in, "files");
    JsonArray *files = json_array_new();
    GString *missing = g_string_new("");
    for (guint i = 0; a && i < json_array_get_length(a) && i < 4; i++) {
        JsonNode *n = json_array_get_element(a, i);
        if (!JSON_NODE_HOLDS_OBJECT(n)) continue;
        JsonObject *f = json_node_get_object(n);
        char *p = ctx_resolve(ctx, jstr(f, "path"));
        if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) {
            JsonObject *o = jo_new();
            jo_str(o, "path", p);
            jo_str(o, "description", jstr(f, "description") ? jstr(f, "description") : "");
            json_array_add_object_element(files, o);
        } else {
            if (missing->len) g_string_append(missing, ", ");
            g_string_append(missing, p);
        }
        g_free(p);
    }
    ToolResult *r;
    if (!json_array_get_length(files)) {
        r = tool_errf("No existing regular files to present: %s", missing->str);
        json_array_unref(files);
    } else {
        GString *t = g_string_new("");
        g_string_append_printf(t, "Presented %u file(s) to the user.", json_array_get_length(files));
        if (missing->len) g_string_append_printf(t, " Missing: %s", missing->str);
        r = tool_ok(t->str);
        g_string_free(t, TRUE);
        r->meta = meta_new();
        jo_arr(r->meta, "files", files);
    }
    g_string_free(missing, TRUE);
    return r;
}

/* ======================= jobs ======================= */

static ToolResult *run_job_list(JsonObject *in, ToolCtx *ctx) {
    char *s = jobs_list(ctx->agent->jobs);
    ToolResult *r = tool_ok(s);
    g_free(s);
    return r;
}

static ToolResult *run_job_output(JsonObject *in, ToolCtx *ctx) {
    const char *id = jstr(in, "job_id");
    if (!id) return tool_err("job_id is required");
    gboolean ok = FALSE;
    char *s = jobs_output(ctx->agent->jobs, id, jbool(in, "wait", FALSE), jint(in, "timeout_ms", 0), ctx_cancel(ctx), &ok);
    ToolResult *r = ok ? tool_ok(s) : tool_err(s);
    g_free(s);
    return r;
}

static ToolResult *run_job_kill(JsonObject *in, ToolCtx *ctx) {
    const char *id = jstr(in, "job_id");
    if (!id) return tool_err("job_id is required");
    char *s = jobs_kill(ctx->agent->jobs, id);
    ToolResult *r = tool_ok(s);
    g_free(s);
    return r;
}

/* ======================= subagent ======================= */

typedef struct {
    Agent *parent;
    char *call_id;
    char *prompt;
    char *model;
    char *cwd;
    Effort effort;
    PermissionMode permission;
    int steps;
    gboolean background;
} SubCtx;

static void sub_free(gpointer p) {
    SubCtx *c = p;
    g_free(c->call_id);
    g_free(c->prompt);
    g_free(c->model);
    g_free(c->cwd);
    g_free(c);
}

typedef struct { Agent *parent; char *call_id; int *steps; } ActivityCtx;

typedef struct { Agent *a; char *id; char *text; } ProgressMsg;

static void sub_activity(const char *line, gpointer data) {
    SubCtx *c = data;
    if (c->background) return; /* the parent's tool call already finished */
    c->steps++;
    char *t = g_strdup_printf("Step %d: %s", c->steps, line);
    ToolCtx tc = { c->parent, c->call_id, c->cwd, c->model };
    ctx_progress(&tc, t);
    g_free(t);
}

typedef struct { volatile gint *job_cancel; Agent *child; volatile gint done; } Watch;

static gpointer watch_thread(gpointer p) {
    Watch *w = p;
    while (!g_atomic_int_get(&w->done)) {
        if (w->job_cancel && g_atomic_int_get(w->job_cancel)) g_atomic_int_set(&w->child->cancelled, 1);
        g_usleep(100000);
    }
    return NULL;
}

typedef struct { Agent *parent; Agent *child; gboolean add; } ChildReg;

static void child_reg(ChildReg *r) {
    g_mutex_lock(&r->parent->lock);
    if (r->add) g_ptr_array_add(r->parent->children, r->child);
    else g_ptr_array_remove(r->parent->children, r->child);
    g_mutex_unlock(&r->parent->lock);
}

typedef struct { Agent *child; } FreeChild;

static void free_child_main(gpointer p) {
    FreeChild *f = p;
    Session *s = f->child->session;
    agent_free(f->child);
    session_free(s);
}

static char *sub_body(gpointer data, volatile gint *job_cancel, gboolean *ok) {
    SubCtx *c = data;
    Agent *parent = c->parent;
    Session *s = session_new(c->cwd);
    s->ephemeral = TRUE;
    g_free(s->model);
    s->model = g_strdup(c->model);
    s->effort = c->effort;
    s->permission = c->permission;
    Agent *child = agent_new(s, parent->depth + 1);
    child->parent = parent;
    /* Background runs outlive the delegating call: route approvals to the parent session without a call id. */
    child->parent_call_id = c->background ? NULL : g_strdup(c->call_id);
    child->on_activity = sub_activity;
    child->activity_data = c;
    Message *m = message_new(ROLE_USER);
    message_add(m, block_text(c->prompt));
    g_ptr_array_add(s->messages, m);
    s->running = TRUE;
    ChildReg reg = { parent, child, TRUE };
    if (!c->background) child_reg(&reg);
    if (!c->background && agent_cancelled(parent)) g_atomic_int_set(&child->cancelled, 1);
    Watch w = { job_cancel, child, 0 };
    GThread *wt = job_cancel ? g_thread_new("sub-watch", watch_thread, &w) : NULL;
    char *error = NULL;
    TurnEnd end = agent_loop(child, &error);
    if (wt) { g_atomic_int_set(&w.done, 1); g_thread_join(wt); }
    reg.add = FALSE;
    if (!c->background) child_reg(&reg);
    s->running = FALSE;
    jobs_kill_all(child->jobs);
    char *final = NULL;
    for (int i = s->messages->len - 1; i >= 0 && !final; i--) {
        Message *mm = g_ptr_array_index(s->messages, i);
        if (mm->role == ROLE_ASSISTANT) final = message_text(mm);
    }
    if (!final) final = g_strdup("");
    int steps = session_step_count(s);
    Usage u = session_total_usage(s);
    JsonObject *meta = jo_new();
    jo_int(meta, "steps", steps);
    jo_int(meta, "tokens", usage_total(&u));
    if (!c->background) {
        ToolCtx tc = { parent, c->call_id, c->cwd, c->model };
        ctx_set_meta(&tc, meta);
    } else json_object_unref(meta);
    char *text;
    switch (end) {
    case END_COMPLETED:
        text = *final ? g_strdup(final) : g_strdup("(the subagent returned no text)");
        *ok = TRUE;
        break;
    case END_STOPPED:
        text = g_strconcat("The subagent was interrupted. Partial result:\n", final, NULL);
        *ok = FALSE;
        break;
    default:
        text = g_strdup_printf("The subagent failed: %s\n%s", error ? error : "unknown error", final);
        *ok = FALSE;
        break;
    }
    g_free(final);
    g_free(error);
    FreeChild fc = { child };
    main_sync(free_child_main, &fc);
    return text;
}

static ToolResult *run_subagent(JsonObject *in, ToolCtx *ctx) {
    const char *prompt = jstr(in, "prompt");
    if (str_blank(prompt)) return tool_err("prompt is required");
    const char *label = jstr(in, "description") ? jstr(in, "description") : "Subagent task";
    SubCtx *c = g_new0(SubCtx, 1);
    c->parent = ctx->agent;
    c->call_id = g_strdup(ctx->call_id);
    c->prompt = g_strdup(prompt);
    c->model = g_strdup(ctx->model ? ctx->model : "deepseek-flash");
    c->cwd = g_strdup(ctx->cwd);
    c->effort = ctx->agent->session->effort;
    c->permission = ctx_permission(ctx);
    if (jbool(in, "run_in_background", FALSE)) {
        c->background = TRUE;
        const char *id = jobs_start(ctx->agent->jobs, JOB_SUBAGENT, label, sub_body, c, sub_free);
        ToolResult *r = tool_ok(NULL);
        g_free(r->text);
        r->text = g_strdup_printf("Started subagent as background job %s. Collect its result with job_output.", id);
        r->meta = meta_new();
        jo_str(r->meta, "job", id);
        return r;
    }
    gboolean ok = FALSE;
    char *text = sub_body(c, NULL, &ok);
    sub_free(c);
    ToolResult *r = ok ? tool_ok(text) : tool_err(text);
    g_free(text);
    return r;
}

/* ======================= web ======================= */

static char *decode_entities(const char *s) {
    if (!strchr(s, '&')) return g_strdup(s);
    static const char *map[][2] = {
        { "&nbsp;", " " }, { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&#39;", "'" }, { "&apos;", "'" },
        { "&mdash;", "—" }, { "&ndash;", "–" }, { "&hellip;", "…" }, { "&rsquo;", "’" }, { "&lsquo;", "‘" }, { "&rdquo;", "”" },
        { "&ldquo;", "“" }, { "&copy;", "©" }, { "&reg;", "®" }, { "&trade;", "™" }, { "&middot;", "·" }, { "&bull;", "•" },
    };
    GString *out = g_string_sized_new(strlen(s));
    const char *p = s;
    while (*p) {
        if (*p == '&') {
            gboolean done = FALSE;
            for (size_t i = 0; i < G_N_ELEMENTS(map) && !done; i++) {
                size_t l = strlen(map[i][0]);
                if (!strncmp(p, map[i][0], l)) { g_string_append(out, map[i][1]); p += l; done = TRUE; }
            }
            if (done) continue;
            if (p[1] == '#') {
                const char *q = p + 2;
                gboolean hex = *q == 'x' || *q == 'X';
                if (hex) q++;
                char *end;
                gulong v = strtoul(q, &end, hex ? 16 : 10);
                if (end > q && *end == ';' && v > 0 && v < 0x110000 && g_unichar_validate(v)) {
                    g_string_append_unichar(out, v);
                    p = end + 1;
                    continue;
                }
            }
        }
        g_string_append_c(out, *p++);
    }
    return g_string_free(out, FALSE);
}

static char *html_attr(const char *tag, const char *name) {
    char *pat = g_strdup_printf("\\b%s\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)", name);
    GRegex *rx = g_regex_new(pat, G_REGEX_CASELESS, 0, NULL);
    g_free(pat);
    GMatchInfo *mi = NULL;
    char *res = NULL;
    if (rx && g_regex_match(rx, tag, 0, &mi)) {
        char *v = g_match_info_fetch(mi, 1);
        size_t n = strlen(v);
        if (n >= 2 && (v[0] == '"' || v[0] == '\'')) { v[n - 1] = 0; res = decode_entities(v + 1); }
        else res = decode_entities(v);
        g_free(v);
    }
    if (mi) g_match_info_free(mi);
    if (rx) g_regex_unref(rx);
    return res;
}

static char *html_to_markdown(const char *html, const char *base) {
    char *s = g_strdup(html);
    const char *strip[] = { "script", "style", "noscript", "svg", "head", "template", "iframe", "nav", "footer", NULL };
    for (int i = 0; strip[i]; i++) {
        char *pat = g_strdup_printf("<%s\\b[^>]*>.*?</%s\\s*>", strip[i], strip[i]);
        GRegex *rx = g_regex_new(pat, G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
        g_free(pat);
        if (rx) {
            char *t = g_regex_replace_literal(rx, s, -1, 0, " ", 0, NULL);
            if (t) { g_free(s); s = t; }
            g_regex_unref(rx);
        }
    }
    GRegex *crx = g_regex_new("<!--.*?-->", G_REGEX_DOTALL, 0, NULL);
    if (crx) {
        char *t = g_regex_replace_literal(crx, s, -1, 0, "", 0, NULL);
        if (t) { g_free(s); s = t; }
        g_regex_unref(crx);
    }
    GRegex *ws = g_regex_new("\\s+", 0, 0, NULL);
    GString *out = g_string_sized_new(strlen(s) / 2);
    GPtrArray *links = g_ptr_array_new_with_free_func(g_free);
    int in_pre = 0;
    const char *p = s;
    while (*p) {
        const char *close = *p == '<' ? strchr(p, '>') : NULL;
        if (close) {
            char *raw = g_strndup(p + 1, close - p - 1);
            p = close + 1;
            gboolean closing = raw[0] == '/';
            const char *ns = raw + (closing ? 1 : 0);
            size_t nl = 0;
            while (ns[nl] && !g_ascii_isspace(ns[nl]) && ns[nl] != '/' && ns[nl] != '>') nl++;
            char *name = g_ascii_strdown(ns, nl);
            if (!strcmp(name, "br")) g_string_append_c(out, '\n');
            else if (!strcmp(name, "p") || !strcmp(name, "div") || !strcmp(name, "section") || !strcmp(name, "article") ||
                     !strcmp(name, "main") || !strcmp(name, "header") || !strcmp(name, "table") || !strcmp(name, "ul") ||
                     !strcmp(name, "ol") || !strcmp(name, "blockquote") || !strcmp(name, "figure") || !strcmp(name, "form") || !strcmp(name, "dl"))
                g_string_append(out, "\n\n");
            else if (!strcmp(name, "tr") || !strcmp(name, "dt") || !strcmp(name, "dd")) g_string_append_c(out, '\n');
            else if (!strcmp(name, "td") || !strcmp(name, "th")) { if (closing) g_string_append(out, " | "); }
            else if (!strcmp(name, "li")) { if (!closing) g_string_append(out, "\n- "); }
            else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
                if (closing) g_string_append(out, "\n\n");
                else {
                    g_string_append(out, "\n\n");
                    for (int k = 0; k < name[1] - '0'; k++) g_string_append_c(out, '#');
                    g_string_append_c(out, ' ');
                }
            } else if (!strcmp(name, "pre")) {
                if (closing) in_pre = MAX(0, in_pre - 1);
                else in_pre++;
                g_string_append(out, "\n```\n");
            } else if (!strcmp(name, "code")) { if (!in_pre) g_string_append_c(out, '`'); }
            else if (!strcmp(name, "strong") || !strcmp(name, "b")) g_string_append(out, "**");
            else if (!strcmp(name, "em") || !strcmp(name, "i")) g_string_append_c(out, '*');
            else if (!strcmp(name, "hr")) g_string_append(out, "\n\n---\n\n");
            else if (!strcmp(name, "a")) {
                if (closing) {
                    if (links->len) {
                        char *href = g_ptr_array_steal_index(links, links->len - 1);
                        if (href && *href) g_string_append_printf(out, "](%s)", href);
                        g_free(href);
                    }
                } else {
                    char *href = html_attr(raw, "href");
                    char *abs = NULL;
                    if (href && !g_str_has_prefix(href, "javascript:") && !g_str_has_prefix(href, "#"))
                        abs = g_uri_resolve_relative(base, href, G_URI_FLAGS_NONE, NULL);
                    if (abs) g_string_append_c(out, '[');
                    g_ptr_array_add(links, abs ? abs : g_strdup(""));
                    g_free(href);
                }
            } else if (!strcmp(name, "img")) {
                char *alt = html_attr(raw, "alt");
                if (alt && *alt) g_string_append_printf(out, "[image: %s]", alt);
                g_free(alt);
            }
            g_free(name);
            g_free(raw);
        } else {
            const char *next = strchr(p + (*p == '<' ? 1 : 0), '<');
            if (!next) next = p + strlen(p);
            char *chunk = g_strndup(p, next - p);
            char *dec = decode_entities(chunk);
            g_free(chunk);
            if (!in_pre) {
                char *c2 = g_regex_replace_literal(ws, dec, -1, 0, " ", 0, NULL);
                g_free(dec);
                dec = c2;
            }
            g_string_append(out, dec);
            g_free(dec);
            p = next;
        }
    }
    g_regex_unref(ws);
    g_ptr_array_unref(links);
    g_free(s);
    /* Normalize whitespace. */
    char **lines = g_strsplit(out->str, "\n", -1);
    g_string_free(out, TRUE);
    GString *res = g_string_new("");
    int blank = 0;
    for (int i = 0; lines[i]; i++) {
        char *l = g_strstrip(lines[i]);
        if (!*l) {
            blank++;
            if (blank <= 1) g_string_append_c(res, '\n');
        } else {
            blank = 0;
            g_string_append(res, l);
            g_string_append_c(res, '\n');
        }
    }
    g_strfreev(lines);
    char *r = str_trim(res->str);
    g_string_free(res, TRUE);
    return r;
}

typedef struct { GString *body; gsize cap; volatile gint *cancel; } FetchBuf;

static size_t fetch_write(char *ptr, size_t size, size_t nmemb, void *ud) {
    FetchBuf *b = ud;
    size_t len = size * nmemb;
    if (b->body->len < b->cap) g_string_append_len(b->body, ptr, MIN(len, b->cap - b->body->len));
    return b->body->len >= b->cap ? 0 : len;
}

static int fetch_progress(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    FetchBuf *f = ud;
    return f->cancel && g_atomic_int_get(f->cancel) ? 1 : 0;
}

static ToolResult *run_web_fetch(JsonObject *in, ToolCtx *ctx) {
    const char *url = jstr(in, "url");
    if (!url || !(g_str_has_prefix(url, "http://") || g_str_has_prefix(url, "https://"))) return tool_err("A valid http(s) URL is required.");
    CURL *c = curl_easy_init();
    FetchBuf buf = { g_string_new(""), 8000000, ctx_cancel(ctx) };
    struct curl_slist *h = curl_slist_append(NULL, "Accept: text/html,application/xhtml+xml,text/plain,application/json;q=0.9,*/*;q=0.8");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0 Safari/537.36");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, fetch_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, fetch_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &buf);
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    char *ctype = NULL, *eff = NULL;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ctype);
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
    char *type = g_ascii_strdown(ctype ? ctype : "", -1);
    char *final_url = g_strdup(eff ? eff : url);
    ToolResult *r;
    if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && buf.body->len >= buf.cap)) {
        r = tool_errf("Fetch failed: %s", curl_easy_strerror(rc));
    } else if (g_str_has_prefix(type, "image/") || strstr(type, "pdf") || strstr(type, "octet-stream") || g_str_has_prefix(type, "video/") ||
               g_str_has_prefix(type, "audio/")) {
        r = tool_errf("%s returned binary content (%s, %zu bytes) that cannot be decoded to text.", final_url, type, buf.body->len);
    } else {
        char *body = utf8_valid_dup(buf.body->str, buf.body->len);
        char *text;
        gboolean html = strstr(type, "html") != NULL;
        if (!html && !*type) {
            char *head = g_ascii_strdown(body, MIN(strlen(body), 512));
            html = strstr(head, "<html") != NULL || strstr(head, "<!doctype html") != NULL;
            g_free(head);
        }
        if (html) { text = html_to_markdown(body, final_url); g_free(body); }
        else text = body;
        GString *out = g_string_new("");
        g_string_append_printf(out, "URL: %s\nStatus: %ld\n\n", final_url, status);
        if (g_utf8_strlen(text, -1) > 100000) {
            char *saved = spill_write(text, "fetch");
            char *pre = utf8_prefix(text, 100000);
            g_free(text);
            text = pre;
            g_string_append(out, "[content truncated to 100000 characters");
            if (saved) g_string_append_printf(out, "; full text saved to %s", saved);
            g_string_append(out, "]\n\n");
            g_free(saved);
        }
        g_string_append(out, text);
        g_free(text);
        r = status >= 400 ? tool_err(out->str) : tool_ok(out->str);
        g_string_free(out, TRUE);
        r->meta = meta_new();
        jo_int(r->meta, "status", status);
    }
    g_free(type);
    g_free(final_url);
    g_string_free(buf.body, TRUE);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return r;
}

typedef struct {
    char *query;
    char *model;
    volatile gint *cancel;
    char *answer;
    GPtrArray *sources; /* pairs: title, url */
    char *error;
} SearchJob;

static gpointer search_thread(gpointer p) {
    SearchJob *j = p;
    JsonObject *o = jo_new();
    jo_str(o, "model", j->model);
    jo_int(o, "max_tokens", 4096);
    jo_bool(o, "stream", FALSE);
    JsonArray *msgs = json_array_new();
    JsonObject *m = jo_new();
    jo_str(m, "role", "user");
    JsonArray *content = json_array_new();
    JsonObject *t = jo_new();
    jo_str(t, "type", "text");
    char *q = g_strdup_printf("Perform a web search for the query: %s", j->query);
    jo_str(t, "text", q);
    g_free(q);
    json_array_add_object_element(content, t);
    jo_arr(m, "content", content);
    json_array_add_object_element(msgs, m);
    jo_arr(o, "messages", msgs);
    JsonArray *tools = json_array_new();
    JsonObject *tool = jo_new();
    jo_str(tool, "type", "web_search_20250305");
    jo_str(tool, "name", "web_search");
    jo_int(tool, "max_uses", 5);
    json_array_add_object_element(tools, tool);
    jo_arr(o, "tools", tools);
    char *body = jobj_to_str(o);
    json_object_unref(o);
    LLMError *err = NULL;
    JsonNode *resp = llm_complete(body, NULL, j->cancel, &err);
    g_free(body);
    j->sources = g_ptr_array_new_with_free_func(g_free);
    if (!resp) {
        j->error = g_strdup(err ? err->message : "request failed");
        llm_error_free(err);
        return NULL;
    }
    GString *ans = g_string_new("");
    JsonArray *blocks = jarr(json_node_get_object(resp), "content");
    for (guint i = 0; blocks && i < json_array_get_length(blocks); i++) {
        JsonNode *bn = json_array_get_element(blocks, i);
        if (!JSON_NODE_HOLDS_OBJECT(bn)) continue;
        JsonObject *b = json_node_get_object(bn);
        const char *type = jstr(b, "type");
        if (!g_strcmp0(type, "web_search_tool_result")) {
            JsonArray *items = jarr(b, "content");
            for (guint k = 0; items && k < json_array_get_length(items); k++) {
                JsonNode *in = json_array_get_element(items, k);
                if (!JSON_NODE_HOLDS_OBJECT(in)) continue;
                JsonObject *it = json_node_get_object(in);
                if (g_strcmp0(jstr(it, "type"), "web_search_result") || str_blank(jstr(it, "url"))) continue;
                g_ptr_array_add(j->sources, g_strdup(jstr(it, "title") ? jstr(it, "title") : jstr(it, "url")));
                g_ptr_array_add(j->sources, g_strdup(jstr(it, "url")));
            }
        } else if (!g_strcmp0(type, "text") && jstr(b, "text")) {
            g_string_append(ans, jstr(b, "text"));
        }
    }
    json_node_unref(resp);
    j->answer = str_trim(ans->str);
    g_string_free(ans, TRUE);
    if (!j->sources->len && !*j->answer) j->error = g_strdup("DeepSeek returned no web search results");
    return NULL;
}

static ToolResult *run_web_search(JsonObject *in, ToolCtx *ctx) {
    GPtrArray *queries = g_ptr_array_new();
    JsonArray *qa = jarr(in, "queries");
    for (guint i = 0; qa && i < json_array_get_length(qa) && queries->len < 4; i++) {
        const char *q = json_node_get_string(json_array_get_element(qa, i));
        if (q && *q) g_ptr_array_add(queries, (gpointer)q);
    }
    if (!queries->len && jstr(in, "query")) g_ptr_array_add(queries, (gpointer)jstr(in, "query"));
    if (!queries->len) { g_ptr_array_unref(queries); return tool_err("queries must contain 1–4 search queries."); }
    guint n = queries->len;
    SearchJob *jobs = g_new0(SearchJob, n);
    GThread **th = g_new(GThread *, n);
    for (guint i = 0; i < n; i++) {
        jobs[i].query = g_ptr_array_index(queries, i);
        jobs[i].model = (char *)(ctx->model ? ctx->model : "deepseek-flash");
        jobs[i].cancel = ctx_cancel(ctx);
        th[i] = g_thread_new("search", search_thread, &jobs[i]);
    }
    for (guint i = 0; i < n; i++) g_thread_join(th[i]);
    g_free(th);
    GString *answers = g_string_new("");
    GString *src = g_string_new("");
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    JsonArray *meta_sources = json_array_new();
    int count = 0;
    const char *first_error = NULL;
    for (guint i = 0; i < n; i++) {
        SearchJob *j = &jobs[i];
        if (j->error && !first_error) first_error = j->error;
        if (j->answer && *j->answer) g_string_append_printf(answers, "%s%s", answers->len ? "\n\n" : "", j->answer);
        for (guint k = 0; j->sources && k + 1 < j->sources->len; k += 2) {
            const char *title = g_ptr_array_index(j->sources, k), *url = g_ptr_array_index(j->sources, k + 1);
            if (g_hash_table_contains(seen, url)) continue;
            g_hash_table_add(seen, (gpointer)url);
            count++;
            g_string_append_printf(src, "\n%d. [%s](%s)", count, title, url);
            if (count <= 20) {
                JsonObject *o = jo_new();
                jo_str(o, "title", title);
                jo_str(o, "url", url);
                json_array_add_object_element(meta_sources, o);
            }
        }
    }
    ToolResult *r;
    if (!count && !answers->len) {
        r = tool_errf("Web search failed: %s", first_error ? first_error : "no results");
        json_array_unref(meta_sources);
    } else {
        GString *text = g_string_new("");
        if (answers->len) g_string_append_printf(text, "Summary:\n%s\n\n", answers->str);
        g_string_append_printf(text, "Sources:%s", src->str);
        r = tool_ok(text->str);
        g_string_free(text, TRUE);
        r->meta = meta_new();
        jo_arr(r->meta, "sources", meta_sources);
    }
    g_hash_table_unref(seen);
    g_string_free(answers, TRUE);
    g_string_free(src, TRUE);
    for (guint i = 0; i < n; i++) {
        g_free(jobs[i].answer);
        g_free(jobs[i].error);
        if (jobs[i].sources) g_ptr_array_unref(jobs[i].sources);
    }
    g_free(jobs);
    g_ptr_array_unref(queries);
    return r;
}

/* ======================= registry ======================= */

static const ToolDef TOOLS[] = {
    { "bash",
      "Execute a bash command (`bash -c`) and return its stdout/stderr. Each call runs in a fresh shell; pass `workdir` instead of using `cd`. "
      "Managed `$DSH_*` variables expose current harness environment facts. Long output is truncated to its tail; the full output is saved to a "
      "file whose path is reported when available. Commands may run under a file sandbox; a blocked file operation is reported as `Permission "
      "denied`, a policy denial: do not retry another way.",
      "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"The bash command to execute.\"},\"description\":{\"type\":"
      "\"string\",\"description\":\"Clear, concise description of what this command does in active voice, 5-10 words (shown in the UI). Examples: "
      "\\\"ls\\\" → \\\"List files in current directory\\\"; \\\"git status\\\" → \\\"Show working tree status\\\"; \\\"npm install\\\" → \\\"Install "
      "package dependencies\\\".\"},\"timeoutMs\":{\"type\":\"number\",\"description\":\"Timeout in milliseconds. The executor applies its configured "
      "default and cap; on expiry the command moves to the background as a job instead of being killed.\"},\"workdir\":{\"type\":\"string\","
      "\"description\":\"Working directory for this command. Defaults to the session workspace; a relative path is resolved against it.\"},"
      "\"run_in_background\":{\"type\":\"boolean\",\"description\":\"Run in the background and return a job id immediately (collect with job_output, "
      "stop with job_kill). No timeout applies.\"}},\"required\":[\"command\",\"description\"]}",
      FALSE, run_bash },
    { "read", "Read a UTF-8 text file and return line-numbered content.",
      "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\",\"description\":\"Path to read, resolved by the filesystem backend.\"},"
      "\"offset\":{\"type\":\"number\",\"description\":\"1-based first line to return. Defaults to 1.\"},\"limit\":{\"type\":\"number\",\"description\":"
      "\"Maximum number of lines to return. Defaults to 2000.\"}},\"required\":[\"file_path\"]}",
      TRUE, run_read },
    { "write", "Create or fully replace a UTF-8 text file.",
      "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\",\"description\":\"Path to write, resolved by the filesystem backend.\"},"
      "\"content\":{\"type\":\"string\",\"description\":\"Full UTF-8 text content to write.\"}},\"required\":[\"file_path\",\"content\"]}",
      FALSE, run_write },
    { "edit", "Edit an existing UTF-8 text file by replacing literal text.",
      "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\",\"description\":\"Path to edit, resolved by the filesystem backend.\"},"
      "\"old_string\":{\"type\":\"string\",\"description\":\"Literal text to replace.\"},\"new_string\":{\"type\":\"string\",\"description\":\"Literal "
      "replacement text. Use an empty string to delete the match.\"},\"replace_all\":{\"type\":\"boolean\",\"description\":\"Replace all matches. "
      "Defaults to false; when false, old_string must appear exactly once.\"}},\"required\":[\"file_path\",\"old_string\",\"new_string\"]}",
      FALSE, run_edit },
    { "read_image",
      "Read a PNG/JPEG/WebP/GIF file and return the image itself. Large images are downscaled automatically; do not install image libraries or "
      "create thumbnails to inspect an image.",
      "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\",\"description\":\"Path to the image file, resolved by the filesystem "
      "backend.\"}},\"required\":[\"file_path\"]}",
      TRUE, run_read_image },
    { "glob",
      "Find files, not directories, whose paths match a glob pattern, including hidden and ignored files. Returns up to 100 paths in "
      "modification-time order; a larger result is sampled across top-level entries and reports where the complete list was saved.",
      "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Glob pattern to match file paths against (e.g. "
      "\\\"**/*.ts\\\", \\\"src/**/*.test.js\\\"). A pattern with no \\\"/\\\" matches the basename at any depth, so \\\"*\\\" and \\\"*.ts\\\" both "
      "search the whole tree; include a separator to anchor the depth.\"},\"path\":{\"type\":\"string\",\"description\":\"Directory to search in. "
      "Defaults to the session workspace; a relative path resolves against it.\"}},\"required\":[\"pattern\"]}",
      TRUE, run_glob },
    { "grep",
      "Search file contents with a ripgrep regular expression. Returns matching lines with line numbers, grouped by file. Returns up to 250 "
      "matches; a larger result reports where the complete match list was saved.",
      "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Regular expression to search for (ripgrep syntax).\"},"
      "\"path\":{\"type\":\"string\",\"description\":\"File or directory to search. Defaults to the session workspace; a relative path resolves "
      "against it.\"},\"include\":{\"type\":\"string\",\"description\":\"One glob filter for which files to search (e.g. \\\"*.ts\\\", "
      "\\\"*.{js,jsx}\\\"). Not a list; negation is not supported.\"}},\"required\":[\"pattern\"]}",
      TRUE, run_grep },
    { "todo_write",
      "Record and update a task list to plan multi-step work and show progress; skip it for trivial single-step tasks. Add one todo per concrete "
      "step before you start. While work remains, keep the todos being worked on `in_progress`, several only when work runs in parallel. Mark each "
      "todo `completed` as soon as it is done.",
      "{\"type\":\"object\",\"properties\":{\"todos\":{\"type\":\"array\",\"description\":\"The COMPLETE task list, replacing any previous list.\","
      "\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"content\":{\"type\":\"string\",\"description\":\"What the task "
      "is — a short imperative line.\"},\"status\":{\"type\":\"string\",\"description\":\"pending (not started) | in_progress (now) | completed "
      "(done).\",\"enum\":[\"pending\",\"in_progress\",\"completed\"]}},\"required\":[\"content\",\"status\"]}}},\"required\":[\"todos\"]}",
      FALSE, run_todo_write },
    { "web_fetch", "Fetch the content of a specific HTTP(S) URL and return it decoded to text.",
      "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"The HTTP(S) URL to fetch.\"}},\"required\":[\"url\"]}",
      TRUE, run_web_fetch },
    { "job_list", "List your background jobs (running and finished) with their ids, kinds, and statuses.", "{\"type\":\"object\",\"properties\":{}}",
      TRUE, run_job_list },
    { "job_output",
      "Read a background job: output since the previous read for stream jobs, or the result of a finished final-output job.",
      "{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"Job id returned by the tool that started the background "
      "work.\"},\"wait\":{\"type\":\"boolean\",\"description\":\"Block until the job finishes or the timeout expires; a timed-out wait leaves the job "
      "running. Defaults to false.\"},\"timeout_ms\":{\"type\":\"number\",\"description\":\"Max wait in milliseconds with wait: true. Defaults to and "
      "is capped by configuration.\"}},\"required\":[\"job_id\"]}",
      TRUE, run_job_output },
    { "job_kill", "Request cancellation of a running background job.",
      "{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"Job id returned by the tool that started the background "
      "work.\"},\"reason\":{\"type\":\"string\",\"description\":\"Optional short reason, recorded in the log and forwarded to the job.\"}},"
      "\"required\":[\"job_id\"]}",
      FALSE, run_job_kill },
};

static const ToolDef WEB_SEARCH = {
    "web_search", "Search the web for current information. Returns an optional summary answer and a list of source URLs.",
    "{\"type\":\"object\",\"properties\":{\"queries\":{\"type\":\"array\",\"description\":\"1–4 search queries; their results are merged.\",\"items\":"
    "{\"type\":\"string\"}}},\"required\":[\"queries\"]}",
    TRUE, run_web_search
};

static const ToolDef ASK = {
    "ask_user_question", "Ask the user a concise question when you need confirmation, a choice, or missing information before proceeding.",
    "{\"type\":\"object\",\"properties\":{\"questions\":{\"type\":\"array\",\"description\":\"Questions to ask the user before continuing.\",\"items\":"
    "{\"type\":\"object\",\"additionalProperties\":true,\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"Stable id for this question; "
    "echoed in the answer.\"},\"question\":{\"type\":\"string\",\"description\":\"The specific question to ask the user.\"},\"header\":{\"type\":"
    "\"string\",\"description\":\"Optional short heading for the question, such as \\\"Confirm\\\" or \\\"Choose Mode\\\".\"},\"options\":{\"type\":"
    "\"array\",\"description\":\"Optional choices to show the user. If you recommend one, put it first and append \\\"(Recommended)\\\" to that "
    "label.\",\"items\":{\"type\":\"object\",\"additionalProperties\":true,\"properties\":{\"label\":{\"type\":\"string\",\"description\":\"Short "
    "user-facing option label.\"},\"description\":{\"type\":\"string\",\"description\":\"One sentence explaining the tradeoff or impact.\"}},"
    "\"required\":[\"label\"]}},\"multi_select\":{\"type\":\"boolean\",\"description\":\"Whether the user may select more than one option. Defaults "
    "to false.\"}},\"required\":[\"id\",\"question\"]}}},\"required\":[\"questions\"]}",
    FALSE, run_ask
};

static const ToolDef PRESENT = {
    "present",
    "Declare existing files as final deliverables for the user. Use it when the user needs a separate file, especially Office documents, "
    "spreadsheets, and slide decks; prefer your final response when that suffices. The user opens the current files; their contents are not copied.",
    "{\"type\":\"object\",\"properties\":{\"files\":{\"type\":\"array\",\"description\":\"Usually the 1-2 most important deliverables; at most 4 per "
    "call.\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path of an "
    "existing regular file. Relative paths use the Session working directory.\"},\"description\":{\"type\":\"string\",\"description\":\"Brief "
    "description for the user.\"}},\"required\":[\"path\"]}}},\"required\":[\"files\"]}",
    FALSE, run_present
};

static const ToolDef SUBAGENT = {
    "subagent",
    "Delegate a self-contained task to a subagent (a separate agent that works in its own context) to offload focused, independent work — "
    "research, a scoped implementation, an analysis — so it does not consume this conversation's context. The subagent returns its result, not "
    "its intermediate steps. This call waits for the result by default.",
    "{\"type\":\"object\",\"properties\":{\"description\":{\"type\":\"string\",\"description\":\"A short (3-5 word) description of the delegated "
    "task, for display.\"},\"prompt\":{\"type\":\"string\",\"description\":\"The complete, self-contained task for the subagent. It does not share "
    "this conversation's context, so include everything it needs.\"},\"run_in_background\":{\"type\":\"boolean\",\"description\":\"Run as a "
    "background job and return its id (collect with job_output, stop with job_kill). Defaults to false.\"}},\"required\":[\"description\",\"prompt\"]}",
    TRUE, run_subagent
};

static gint cmp_tool(gconstpointer a, gconstpointer b) {
    return strcmp((*(const ToolDef **)a)->name, (*(const ToolDef **)b)->name);
}

GPtrArray *tool_registry(int depth) {
    GPtrArray *t = g_ptr_array_new();
    for (size_t i = 0; i < G_N_ELEMENTS(TOOLS); i++) g_ptr_array_add(t, (gpointer)&TOOLS[i]);
    Settings *s = settings();
    if (s->web_search) g_ptr_array_add(t, (gpointer)&WEB_SEARCH);
    if (depth == 0) {
        g_ptr_array_add(t, (gpointer)&ASK);
        g_ptr_array_add(t, (gpointer)&PRESENT);
        if (s->subagents) g_ptr_array_add(t, (gpointer)&SUBAGENT);
    }
    g_ptr_array_sort(t, cmp_tool);
    return t;
}

const ToolDef *tool_find(GPtrArray *tools, const char *name) {
    for (guint i = 0; name && i < tools->len; i++) {
        const ToolDef *t = g_ptr_array_index(tools, i);
        if (!strcmp(t->name, name)) return t;
    }
    return NULL;
}
