#define _GNU_SOURCE
#include "process.h"
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <linux/types.h>

/* Older glibc headers (e.g. Ubuntu 22.04) may lack these; the numbers are the same on all architectures. */
#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#endif
#ifndef SYS_landlock_add_rule
#define SYS_landlock_add_rule 445
#endif
#ifndef SYS_landlock_restrict_self
#define SYS_landlock_restrict_self 446
#endif
#ifndef SYS_close_range
#define SYS_close_range 436
#endif

#define HEAD_LIMIT (64 * 1024)
#define TAIL_LIMIT (256 * 1024)

struct ChildProcess {
    pid_t pid;
    int fd;
    GMutex lock;
    GCond cond;
    GByteArray *head;
    GByteArray *tail;
    gint64 dropped;
    gint64 total;
    gint64 read_offset;
    int exit_code;
    gboolean exited;
    FILE *spill;
    char *spill_path;
    gboolean spilled;
    void (*on_output)(gpointer);
    gpointer on_output_data;
    gint refs;
};

void proc_ref(ChildProcess *p) { g_atomic_int_inc(&p->refs); }

void proc_unref(ChildProcess *p) {
    if (!p) return;
    if (!g_atomic_int_dec_and_test(&p->refs)) return;
    g_byte_array_unref(p->head);
    g_byte_array_unref(p->tail);
    if (p->spill) fclose(p->spill);
    g_free(p->spill_path);
    g_mutex_clear(&p->lock);
    g_cond_clear(&p->cond);
    g_free(p);
}

int proc_pid(ChildProcess *p) { return p->pid; }

static void append_tail(ChildProcess *p, const guint8 *d, gsize n) {
    g_byte_array_append(p->tail, d, n);
    if (p->tail->len > TAIL_LIMIT) {
        guint drop = p->tail->len - TAIL_LIMIT;
        g_byte_array_remove_range(p->tail, 0, drop);
        p->dropped += drop;
    }
}

static void ingest(ChildProcess *p, const guint8 *d, gsize n) {
    g_mutex_lock(&p->lock);
    p->total += n;
    if (p->head->len < HEAD_LIMIT) {
        gsize take = MIN((gsize)(HEAD_LIMIT - p->head->len), n);
        g_byte_array_append(p->head, d, take);
        if (take < n) append_tail(p, d + take, n - take);
    } else {
        append_tail(p, d, n);
    }
    if (p->total > HEAD_LIMIT + TAIL_LIMIT) {
        if (!p->spilled) {
            p->spilled = TRUE;
            p->spill = fopen(p->spill_path, "w");
            if (p->spill) {
                fwrite(p->head->data, 1, p->head->len, p->spill);
                fwrite(p->tail->data, 1, p->tail->len, p->spill);
            }
        } else if (p->spill) {
            fwrite(d, 1, n, p->spill);
        }
    }
    void (*cb)(gpointer) = p->on_output;
    gpointer cbd = p->on_output_data;
    g_mutex_unlock(&p->lock);
    if (cb) cb(cbd);
}

static gpointer reader_thread(gpointer data) {
    ChildProcess *p = data;
    guint8 buf[65536];
    for (;;) {
        ssize_t n = read(p->fd, buf, sizeof buf);
        if (n > 0) { ingest(p, buf, n); continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(p->fd);
    g_mutex_lock(&p->lock);
    if (p->spill) fflush(p->spill);
    g_mutex_unlock(&p->lock);
    proc_unref(p);
    return NULL;
}

static gpointer waiter_thread(gpointer data) {
    ChildProcess *p = data;
    int status = 0;
    while (waitpid(p->pid, &status, 0) < 0 && errno == EINTR) {}
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    /* Let the pipe drain briefly (grandchildren may still hold it open). */
    g_usleep(30000);
    g_mutex_lock(&p->lock);
    p->exit_code = code;
    p->exited = TRUE;
    if (p->spill) fflush(p->spill);
    g_cond_broadcast(&p->cond);
    g_mutex_unlock(&p->lock);
    proc_unref(p);
    return NULL;
}

/* Minimal Landlock ABI definitions (avoids depending on recent kernel headers). */
struct dsn_ruleset_attr { __u64 handled_access_fs; };
struct dsn_path_beneath { __u64 allowed_access; __s32 parent_fd; } __attribute__((packed));
#define LL_WRITE_FILE  (1ULL << 1)
#define LL_REMOVE_DIR  (1ULL << 4)
#define LL_REMOVE_FILE (1ULL << 5)
#define LL_MAKE_CHAR   (1ULL << 6)
#define LL_MAKE_DIR    (1ULL << 7)
#define LL_MAKE_REG    (1ULL << 8)
#define LL_MAKE_SOCK   (1ULL << 9)
#define LL_MAKE_FIFO   (1ULL << 10)
#define LL_MAKE_BLOCK  (1ULL << 11)
#define LL_MAKE_SYM    (1ULL << 12)
#define LL_REFER       (1ULL << 13)
#define LL_TRUNCATE    (1ULL << 14)
#define LL_RULE_PATH_BENEATH 1
#define LL_CREATE_RULESET_VERSION (1U << 0)

int sandbox_abi(void) {
    static int abi = -100;
    if (abi == -100) {
        long r = syscall(SYS_landlock_create_ruleset, NULL, 0, LL_CREATE_RULESET_VERSION);
        abi = r < 0 ? 0 : (int)r;
        if (g_getenv("DSN_NO_SANDBOX")) abi = 0;
    }
    return abi;
}

gboolean sandbox_available(void) { return sandbox_abi() >= 1; }

static void add_rule(int ruleset, const char *path, __u64 handled) {
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    __u64 allowed = handled;
    if (fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode)) allowed &= (LL_WRITE_FILE | LL_TRUNCATE);
    struct dsn_path_beneath pb = { .allowed_access = allowed, .parent_fd = fd };
    if (syscall(SYS_landlock_add_rule, ruleset, LL_RULE_PATH_BENEATH, &pb, 0) != 0) dlog("landlock rule %s failed: %s", path, strerror(errno));
    close(fd);
}

int sandbox_ruleset(PermissionMode mode, const char *cwd) {
    if (mode == PERM_FULL_ACCESS) return -1;
    int abi = sandbox_abi();
    if (abi < 1) return -2;
    __u64 handled = LL_WRITE_FILE | LL_REMOVE_DIR | LL_REMOVE_FILE | LL_MAKE_CHAR | LL_MAKE_DIR | LL_MAKE_REG |
                    LL_MAKE_SOCK | LL_MAKE_FIFO | LL_MAKE_BLOCK | LL_MAKE_SYM;
    if (abi >= 2) handled |= LL_REFER;
    if (abi >= 3) handled |= LL_TRUNCATE;
    struct dsn_ruleset_attr attr = { .handled_access_fs = handled };
    int rs = syscall(SYS_landlock_create_ruleset, &attr, sizeof attr, 0);
    if (rs < 0) return -2;
    const char *home = g_get_home_dir();
    const char *fixed[] = { "/dev", "/tmp", "/var/tmp", "/dev/shm", NULL };
    for (int i = 0; fixed[i]; i++) add_rule(rs, fixed[i], handled);
    const char *tmpdir = g_getenv("TMPDIR");
    if (tmpdir && *tmpdir) add_rule(rs, tmpdir, handled);
    const char *rt = g_get_user_runtime_dir();
    if (rt && *rt) add_rule(rs, rt, handled);
    const char *caches[] = { ".cache", ".npm", ".pnpm-store", ".local/share/pnpm", ".yarn", ".bun", ".cargo", ".rustup",
                             "go", ".gradle", ".m2", ".node-gyp", ".deno", ".config/configstore", ".local/state",
                             ".local/share/mise", ".pyenv", ".ccache", ".electron-gyp", NULL };
    for (int i = 0; caches[i]; i++) {
        char *p = g_build_filename(home, caches[i], NULL);
        add_rule(rs, p, handled);
        g_free(p);
    }
    if (mode == PERM_WORKSPACE_WRITE && cwd) add_rule(rs, cwd, handled);
    return rs;
}

ChildProcess *proc_spawn(char **argv, const char *cwd, char **envp, int sandbox_fd, GError **err) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "pipe failed: %s", g_strerror(errno));
        return NULL;
    }
    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        close(fds[0]);
        close(fds[1]);
        if (devnull >= 0) close(devnull);
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "fork failed: %s", g_strerror(e));
        return NULL;
    }
    if (pid == 0) {
        /* Child: only async-signal-safe calls from here on. */
        setpgid(0, 0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_DFL;
        for (int s = 1; s < 65; s++) sigaction(s, &sa, NULL);
        if (devnull >= 0) dup2(devnull, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        /* Keep inherited descriptors (sockets, files) out of the command. */
        syscall(SYS_close_range, 3U, ~0U, 1U << 2 /* CLOSE_RANGE_CLOEXEC */);
        if (cwd && chdir(cwd) != 0) {
            static const char msg[] = "failed to enter working directory\n";
            ssize_t w = write(2, msg, sizeof msg - 1);
            (void)w;
            _exit(127);
        }
        if (sandbox_fd >= 0) {
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || syscall(SYS_landlock_restrict_self, sandbox_fd, 0) != 0) {
                static const char msg[] = "sandbox setup failed\n";
                ssize_t w = write(2, msg, sizeof msg - 1);
                (void)w;
                _exit(126);
            }
        }
        execve(argv[0], argv, envp);
        static const char msg[] = "exec failed\n";
        ssize_t w = write(2, msg, sizeof msg - 1);
        (void)w;
        _exit(127);
    }
    close(fds[1]);
    if (devnull >= 0) close(devnull);
    ChildProcess *p = g_new0(ChildProcess, 1);
    p->pid = pid;
    p->fd = fds[0];
    p->head = g_byte_array_new();
    p->tail = g_byte_array_new();
    p->exit_code = -1;
    p->refs = 3; /* caller + reader + waiter */
    g_mutex_init(&p->lock);
    g_cond_init(&p->cond);
    char *u = short_uuid();
    char *fn = g_strdup_printf("out-%d-%s.log", pid, u);
    p->spill_path = g_build_filename(spill_dir(), fn, NULL);
    g_free(fn);
    g_free(u);
    g_thread_unref(g_thread_new("proc-read", reader_thread, p));
    g_thread_unref(g_thread_new("proc-wait", waiter_thread, p));
    return p;
}

gboolean proc_running(ChildProcess *p) {
    g_mutex_lock(&p->lock);
    gboolean r = !p->exited;
    g_mutex_unlock(&p->lock);
    return r;
}

int proc_exit_code(ChildProcess *p) {
    g_mutex_lock(&p->lock);
    int c = p->exited ? p->exit_code : -1;
    g_mutex_unlock(&p->lock);
    return c;
}

gboolean proc_wait(ChildProcess *p, double timeout, volatile gint *cancel) {
    gint64 deadline = timeout < 0 ? G_MAXINT64 : g_get_monotonic_time() + (gint64)(timeout * G_USEC_PER_SEC);
    g_mutex_lock(&p->lock);
    while (!p->exited) {
        if (cancel && g_atomic_int_get(cancel)) break;
        gint64 now = g_get_monotonic_time();
        if (now >= deadline) break;
        gint64 slice = MIN(deadline, now + 100 * G_TIME_SPAN_MILLISECOND);
        g_cond_wait_until(&p->cond, &p->lock, slice);
    }
    gboolean done = p->exited;
    g_mutex_unlock(&p->lock);
    return done;
}

static gpointer kill_later(gpointer data) {
    ChildProcess *p = data;
    for (int i = 0; i < 20 && proc_running(p); i++) g_usleep(100000);
    if (proc_running(p)) {
        kill(-p->pid, SIGKILL);
        kill(p->pid, SIGKILL);
    }
    proc_unref(p);
    return NULL;
}

void proc_terminate(ChildProcess *p) {
    if (!proc_running(p)) {
        /* Background grandchildren may linger in the group. */
        kill(-p->pid, SIGTERM);
        return;
    }
    kill(-p->pid, SIGTERM);
    kill(p->pid, SIGTERM);
    proc_ref(p);
    g_thread_unref(g_thread_new("proc-kill", kill_later, p));
}

char *proc_output(ChildProcess *p, int max_chars, gboolean *truncated, char **spill_path) {
    g_mutex_lock(&p->lock);
    GString *raw = g_string_sized_new(p->head->len + p->tail->len + 64);
    g_string_append_len(raw, (const char *)p->head->data, p->head->len);
    if (p->dropped > 0) g_string_append_printf(raw, "\n… [%" G_GINT64_FORMAT " bytes omitted] …\n", p->dropped);
    g_string_append_len(raw, (const char *)p->tail->data, p->tail->len);
    gint64 dropped = p->dropped, total = p->total;
    gboolean spilled = p->spilled && p->spill;
    g_mutex_unlock(&p->lock);
    char *text = ansi_strip(raw->str, raw->len);
    g_string_free(raw, TRUE);
    gboolean trunc = dropped > 0;
    if (max_chars > 0 && g_utf8_strlen(text, -1) > max_chars) {
        char *tail = utf8_suffix(text, max_chars);
        char *t = g_strdup_printf("… [output truncated to last %d characters of %" G_GINT64_FORMAT " bytes] …\n%s", max_chars, total, tail);
        g_free(tail);
        g_free(text);
        text = t;
        trunc = TRUE;
    }
    if (truncated) *truncated = trunc;
    if (spill_path) *spill_path = spilled ? g_strdup(p->spill_path) : NULL;
    return text;
}

char *proc_read_new(ChildProcess *p) {
    g_mutex_lock(&p->lock);
    /* Captured bytes: head covers [0, head.len), tail covers [total - tail.len, total). */
    gint64 head_len = p->head->len, tail_len = p->tail->len, total = p->total;
    gint64 tail_start = total - tail_len;
    gint64 from = p->read_offset;
    p->read_offset = total;
    GString *raw = g_string_new("");
    if (from < head_len) {
        g_string_append_len(raw, (const char *)p->head->data + from, head_len - from);
        if (tail_start > head_len) g_string_append_printf(raw, "\n… [%" G_GINT64_FORMAT " bytes omitted] …\n", tail_start - head_len);
        from = tail_start;
    } else if (from < tail_start) {
        g_string_append_printf(raw, "… [%" G_GINT64_FORMAT " bytes omitted] …\n", tail_start - from);
        from = tail_start;
    }
    if (from < total) {
        gint64 off = MAX(0, from - tail_start);
        if (from >= head_len || tail_start >= head_len) g_string_append_len(raw, (const char *)p->tail->data + off, tail_len - off);
    }
    g_mutex_unlock(&p->lock);
    char *text = ansi_strip(raw->str, raw->len);
    g_string_free(raw, TRUE);
    return text;
}

void proc_set_on_output(ChildProcess *p, void (*fn)(gpointer), gpointer data) {
    g_mutex_lock(&p->lock);
    p->on_output = fn;
    p->on_output_data = data;
    g_mutex_unlock(&p->lock);
}

char *proc_run_capture(char **argv, const char *cwd, double timeout, int *exit_code, volatile gint *cancel) {
    char **env = g_get_environ();
    GError *err = NULL;
    ChildProcess *p = proc_spawn(argv, cwd, env, -1, &err);
    g_strfreev(env);
    if (!p) {
        if (exit_code) *exit_code = 127;
        char *m = g_strdup(err ? err->message : "spawn failed");
        g_clear_error(&err);
        return m;
    }
    gboolean done = proc_wait(p, timeout, cancel);
    if (!done) proc_terminate(p);
    if (exit_code) *exit_code = done ? proc_exit_code(p) : -1;
    char *out = proc_output(p, 2000000, NULL, NULL);
    proc_unref(p);
    return out;
}

/* ---------------- shell environment ---------------- */

static GMutex env_lock;
static char *login_path;

static gpointer warm_thread(gpointer d) {
    const char *shell = g_getenv("SHELL");
    if (!shell || !*shell) shell = "/bin/bash";
    char *argv[] = { (char *)shell, "-l", "-c", "printf %s \"$PATH\"", NULL };
    int code = 0;
    char *out = proc_run_capture(argv, g_get_home_dir(), 5, &code, NULL);
    if (code == 0 && out) {
        char *t = str_trim(out);
        /* Login shells may print banners; the PATH is the last line. */
        char *nl = strrchr(t, '\n');
        const char *path = nl ? nl + 1 : t;
        if (*path && strchr(path, '/')) {
            g_mutex_lock(&env_lock);
            g_free(login_path);
            login_path = g_strdup(path);
            g_mutex_unlock(&env_lock);
        }
        g_free(t);
    }
    g_free(out);
    return NULL;
}

void shell_env_warm_up(void) { g_thread_unref(g_thread_new("login-path", warm_thread, NULL)); }

char **shell_environment(const char *cwd, const char *session_id) {
    char **env = g_get_environ();
    g_mutex_lock(&env_lock);
    char *path = g_strdup(login_path ? login_path : g_environ_getenv(env, "PATH"));
    g_mutex_unlock(&env_lock);
    if (!path) path = g_strdup("/usr/local/bin:/usr/bin:/bin");
    env = g_environ_setenv(env, "PATH", path, TRUE);
    g_free(path);
    env = g_environ_setenv(env, "PWD", cwd, TRUE);
    env = g_environ_setenv(env, "TERM", "dumb", TRUE);
    env = g_environ_setenv(env, "PAGER", "cat", TRUE);
    env = g_environ_setenv(env, "GIT_PAGER", "cat", TRUE);
    env = g_environ_setenv(env, "GIT_TERMINAL_PROMPT", "0", TRUE);
    env = g_environ_setenv(env, "NO_COLOR", "1", TRUE);
    env = g_environ_setenv(env, "DSH_WORKSPACE", cwd, TRUE);
    env = g_environ_setenv(env, "DSH_SESSION_ID", session_id ? session_id : "", TRUE);
    env = g_environ_setenv(env, "DSH_APP", "deepseek-native-linux", TRUE);
    env = g_environ_unsetenv(env, "DESKTOP_STARTUP_ID");
    env = g_environ_unsetenv(env, "XDG_ACTIVATION_TOKEN");
    env = g_environ_unsetenv(env, "DEEPSEEK_API_KEY");
    return env;
}

const char *ripgrep_path(void) {
    static char *rg;
    static gboolean looked;
    if (!looked) {
        looked = TRUE;
        rg = g_find_program_in_path("rg");
        if (!rg && g_file_test("/usr/bin/rg", G_FILE_TEST_IS_EXECUTABLE)) rg = g_strdup("/usr/bin/rg");
    }
    return rg;
}
