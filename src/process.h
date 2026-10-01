#pragma once
#include "util.h"
#include "settings.h"

/* A child process in its own process group with merged stdout/stderr captured in memory
 * (bounded: keeps the head and a rolling tail; overflow spills to a file). */
typedef struct ChildProcess ChildProcess;

/* sandbox_fd: Landlock ruleset fd applied in the child, or -1. */
ChildProcess *proc_spawn(char **argv, const char *cwd, char **envp, int sandbox_fd, GError **err);
void proc_ref(ChildProcess *p);
void proc_unref(ChildProcess *p);
int proc_pid(ChildProcess *p);
gboolean proc_running(ChildProcess *p);
int proc_exit_code(ChildProcess *p); /* -1 while running */
/* Waits for exit up to timeout seconds (< 0: forever). Returns TRUE if exited; checks *cancel. */
gboolean proc_wait(ChildProcess *p, double timeout, volatile gint *cancel);
void proc_terminate(ChildProcess *p);
/* Complete captured output (ANSI stripped), tail-truncated to max_chars. */
char *proc_output(ChildProcess *p, int max_chars, gboolean *truncated, char **spill_path);
/* Output produced since the previous call. */
char *proc_read_new(ChildProcess *p);
void proc_set_on_output(ChildProcess *p, void (*fn)(gpointer), gpointer data);

/* Runs argv to completion (with timeout seconds) and returns stdout+stderr. */
char *proc_run_capture(char **argv, const char *cwd, double timeout, int *exit_code, volatile gint *cancel);

/* Environment for shell commands (login PATH, pager settings, DSH_* variables). */
void shell_env_warm_up(void);
char **shell_environment(const char *cwd, const char *session_id);

/* Landlock sandbox: returns a ruleset fd for the mode, -1 when the mode needs none,
 * -2 when Landlock is unavailable. */
int sandbox_ruleset(PermissionMode mode, const char *cwd);
gboolean sandbox_available(void);
int sandbox_abi(void);

const char *ripgrep_path(void);
