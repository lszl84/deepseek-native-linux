/* Checks that shell commands run under the Landlock sandbox can write only where the
 * permission mode allows. Needs no network and no API key: `make test`. */
#include "process.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    PermissionMode mode;
    const char *cmd;
    const char *expect; /* substring the output must contain */
} Case;

static int run_case(const char *ws, const Case *c) {
    int fd = sandbox_ruleset(c->mode, ws);
    char *argv[] = { "/bin/bash", "-c", (char *)c->cmd, NULL };
    char **env = shell_environment(ws, "probe");
    GError *err = NULL;
    ChildProcess *p = proc_spawn(argv, ws, env, fd, &err);
    g_strfreev(env);
    if (fd >= 0) close(fd);
    if (!p) {
        printf("FAIL spawn: %s\n", err ? err->message : "?");
        return 1;
    }
    proc_wait(p, 10, NULL);
    char *out = proc_output(p, 4000, NULL, NULL);
    int ok = strstr(out, c->expect) != NULL;
    printf("%s [%s] %s\n      → %s", ok ? "ok  " : "FAIL", perm_raw(c->mode), c->cmd, out);
    if (!*out || out[strlen(out) - 1] != '\n') printf("\n");
    g_free(out);
    proc_unref(p);
    return !ok;
}

int main(void) {
    main_init();
    if (!sandbox_available()) {
        printf("Landlock is not available on this kernel; commands fall back to approvals. Skipping.\n");
        return 0;
    }
    printf("Landlock ABI %d\n", sandbox_abi());
    /* The workspace must not live under a temp or cache directory: those stay writable in every mode. */
    char *tmpl = g_build_filename(g_get_user_data_dir(), "dsn-probe-XXXXXX", NULL);
    const char *ws = g_mkdtemp(tmpl);
    char *outside = g_build_filename(g_get_home_dir(), ".dsn-sandbox-probe", NULL);
    unlink(outside);
    char *c_out = g_strdup_printf("echo x > %s && echo WROTE || echo BLOCKED", outside);
    char *c_in = g_strdup_printf("echo x > %s/inside.txt && echo WROTE || echo BLOCKED", ws);
    char *c_mv = g_strdup_printf("touch %s/a && mv %s/a /tmp/dsn-probe-moved-$$ && rm /tmp/dsn-probe-moved-$$ && echo MOVED", ws, ws);
    Case cases[] = {
        { PERM_WORKSPACE_WRITE, c_in, "WROTE" },
        { PERM_WORKSPACE_WRITE, c_out, "BLOCKED" },
        { PERM_READ_ONLY, c_in, "BLOCKED" },
        { PERM_READ_ONLY, "echo t > /tmp/dsn-probe-tmp-$$ && rm /tmp/dsn-probe-tmp-$$ && echo TMP-OK", "TMP-OK" },
        { PERM_WORKSPACE_WRITE, "echo hi > /dev/stdout; echo q > /dev/null && echo DEV-OK", "DEV-OK" },
        { PERM_WORKSPACE_WRITE, c_mv, "MOVED" },
        { PERM_FULL_ACCESS, c_out, "WROTE" },
        { PERM_WORKSPACE_WRITE, "printenv DEEPSEEK_API_KEY >/dev/null && echo KEY-VISIBLE || echo KEY-HIDDEN", "KEY-HIDDEN" },
    };
    g_setenv("DEEPSEEK_API_KEY", "dummy-not-a-real-key", TRUE);
    int failures = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) failures += run_case(ws, &cases[i]);
    unlink(outside);
    char *inside = g_build_filename(ws, "inside.txt", NULL);
    unlink(inside);
    rmdir(ws);
    g_free(tmpl);
    g_free(inside);
    g_free(outside);
    g_free(c_out);
    g_free(c_in);
    g_free(c_mv);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
