#include "jobs.h"
#include <string.h>

struct Job {
    char *id;
    JobKind kind;
    char *label;
    double started_at, finished_at;
    JobState state;
    char *result;
    ChildProcess *proc;
    volatile gint cancel;
    gboolean collected;
    JobManager *jm;
    JobBody body;
    gpointer data;
    GDestroyNotify free_data;
};

struct JobManager {
    GMutex lock;
    GCond cond;
    GHashTable *jobs; /* id -> Job* */
    GPtrArray *order; /* Job* */
    int counter;
    void (*on_finish)(const char *, const char *, const char *, const char *, gpointer);
    gpointer on_finish_data;
    int threads; /* live job threads */
    gboolean dying;
};

static const char *kind_raw(JobKind k) { return k == JOB_BASH ? "bash" : "subagent"; }

static const char *state_raw(JobState s) {
    switch (s) {
    case JOB_RUNNING: return "running";
    case JOB_COMPLETED: return "completed";
    case JOB_FAILED: return "failed";
    default: return "killed";
    }
}

static void job_free(gpointer p) {
    Job *j = p;
    g_free(j->id);
    g_free(j->label);
    g_free(j->result);
    if (j->proc) proc_unref(j->proc);
    if (j->free_data && j->data) j->free_data(j->data);
    g_free(j);
}

JobManager *jobs_new(void) {
    JobManager *jm = g_new0(JobManager, 1);
    g_mutex_init(&jm->lock);
    g_cond_init(&jm->cond);
    jm->jobs = g_hash_table_new(g_str_hash, g_str_equal);
    jm->order = g_ptr_array_new_with_free_func(job_free);
    return jm;
}

void jobs_free(JobManager *jm) {
    if (!jm) return;
    jobs_kill_all(jm);
    /* Wait for job threads to drop their references. */
    g_mutex_lock(&jm->lock);
    jm->dying = TRUE;
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (jm->threads > 0 && g_get_monotonic_time() < deadline) g_cond_wait_until(&jm->cond, &jm->lock, deadline);
    gboolean leak = jm->threads > 0;
    g_mutex_unlock(&jm->lock);
    if (leak) return; /* a stuck job still references the manager */
    g_hash_table_unref(jm->jobs);
    g_ptr_array_unref(jm->order);
    g_mutex_clear(&jm->lock);
    g_cond_clear(&jm->cond);
    g_free(jm);
}

void jobs_set_on_finish(JobManager *jm, void (*fn)(const char *, const char *, const char *, const char *, gpointer), gpointer data) {
    jm->on_finish = fn;
    jm->on_finish_data = data;
}

static Job *add_job(JobManager *jm, JobKind kind, const char *label) {
    Job *j = g_new0(Job, 1);
    g_mutex_lock(&jm->lock);
    jm->counter++;
    j->id = g_strdup_printf("%s-%d", kind_raw(kind), jm->counter);
    j->kind = kind;
    j->label = g_strdup(label);
    j->started_at = now_ts();
    j->state = JOB_RUNNING;
    j->jm = jm;
    g_hash_table_insert(jm->jobs, j->id, j);
    g_ptr_array_add(jm->order, j);
    jm->threads++;
    g_mutex_unlock(&jm->lock);
    return j;
}

static void complete(Job *j, JobState state, const char *result) {
    JobManager *jm = j->jm;
    g_mutex_lock(&jm->lock);
    gboolean was_killed = j->state == JOB_KILLED;
    if (j->finished_at > 0) { g_mutex_unlock(&jm->lock); return; }
    j->state = was_killed ? JOB_KILLED : state;
    g_free(j->result);
    j->result = g_strdup(result);
    j->finished_at = now_ts();
    char *id = g_strdup(j->id), *label = g_strdup(j->label);
    const char *kind = kind_raw(j->kind), *st = state_raw(j->state);
    gboolean notify = !was_killed && !jm->dying;
    g_mutex_unlock(&jm->lock);
    if (notify && jm->on_finish) jm->on_finish(id, kind, st, label, jm->on_finish_data);
    g_free(id);
    g_free(label);
}

static void thread_done(JobManager *jm) {
    g_mutex_lock(&jm->lock);
    jm->threads--;
    g_cond_broadcast(&jm->cond);
    g_mutex_unlock(&jm->lock);
}

static gpointer adopt_thread(gpointer data) {
    Job *j = data;
    proc_wait(j->proc, -1, NULL);
    int code = proc_exit_code(j->proc);
    char *r = g_strdup_printf("[exit code: %d]", code);
    complete(j, code == 0 ? JOB_COMPLETED : JOB_FAILED, r);
    g_free(r);
    thread_done(j->jm);
    return NULL;
}

const char *jobs_adopt(JobManager *jm, ChildProcess *p, const char *label) {
    Job *j = add_job(jm, JOB_BASH, label);
    proc_ref(p);
    j->proc = p;
    g_thread_unref(g_thread_new("job", adopt_thread, j));
    return j->id;
}

static gpointer body_thread(gpointer data) {
    Job *j = data;
    gboolean ok = FALSE;
    char *text = j->body(j->data, &j->cancel, &ok);
    complete(j, g_atomic_int_get(&j->cancel) ? JOB_KILLED : ok ? JOB_COMPLETED : JOB_FAILED, text);
    g_free(text);
    thread_done(j->jm);
    return NULL;
}

const char *jobs_start(JobManager *jm, JobKind kind, const char *label, JobBody body, gpointer data, GDestroyNotify free_data) {
    Job *j = add_job(jm, kind, label);
    j->body = body;
    j->data = data;
    j->free_data = free_data;
    g_thread_unref(g_thread_new("job", body_thread, j));
    return j->id;
}

char *jobs_kill(JobManager *jm, const char *id) {
    g_mutex_lock(&jm->lock);
    Job *j = g_hash_table_lookup(jm->jobs, id);
    if (!j) { g_mutex_unlock(&jm->lock); return g_strdup_printf("No job with id %s.", id); }
    if (j->state != JOB_RUNNING) {
        char *r = g_strdup_printf("Job %s already %s.", id, state_raw(j->state));
        g_mutex_unlock(&jm->lock);
        return r;
    }
    j->state = JOB_KILLED;
    g_atomic_int_set(&j->cancel, 1);
    ChildProcess *p = j->proc;
    if (p) proc_ref(p);
    g_mutex_unlock(&jm->lock);
    if (p) { proc_terminate(p); proc_unref(p); }
    return g_strdup_printf("Cancellation requested for job %s.", id);
}

void jobs_kill_all(JobManager *jm) {
    g_mutex_lock(&jm->lock);
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < jm->order->len; i++) {
        Job *j = g_ptr_array_index(jm->order, i);
        if (j->state == JOB_RUNNING) g_ptr_array_add(ids, g_strdup(j->id));
    }
    g_mutex_unlock(&jm->lock);
    for (guint i = 0; i < ids->len; i++) g_free(jobs_kill(jm, g_ptr_array_index(ids, i)));
    g_ptr_array_unref(ids);
}

int jobs_thread_count(JobManager *jm) {
    g_mutex_lock(&jm->lock);
    int n = jm->threads;
    g_mutex_unlock(&jm->lock);
    return n;
}

int jobs_running_count(JobManager *jm) {
    int n = 0;
    g_mutex_lock(&jm->lock);
    for (guint i = 0; i < jm->order->len; i++)
        if (((Job *)g_ptr_array_index(jm->order, i))->state == JOB_RUNNING) n++;
    g_mutex_unlock(&jm->lock);
    return n;
}

char *jobs_list(JobManager *jm) {
    GString *s = g_string_new("");
    g_mutex_lock(&jm->lock);
    for (guint i = 0; i < jm->order->len; i++) {
        Job *j = g_ptr_array_index(jm->order, i);
        double end = j->finished_at > 0 ? j->finished_at : now_ts();
        g_string_append_printf(s, "%s%s\t%s\t%s\t%ds\t%s", i ? "\n" : "", j->id, kind_raw(j->kind), state_raw(j->state), (int)(end - j->started_at), j->label);
    }
    g_mutex_unlock(&jm->lock);
    if (!s->len) g_string_append(s, "No background jobs.");
    return g_string_free(s, FALSE);
}

char *jobs_output(JobManager *jm, const char *id, gboolean wait, gint64 timeout_ms, volatile gint *cancel, gboolean *ok) {
    g_mutex_lock(&jm->lock);
    Job *j = id ? g_hash_table_lookup(jm->jobs, id) : NULL;
    if (!j) {
        g_mutex_unlock(&jm->lock);
        *ok = FALSE;
        return g_strdup_printf("No job with id %s.", id ? id : "(none)");
    }
    JobState st = j->state;
    ChildProcess *p = j->proc;
    if (p) proc_ref(p);
    g_mutex_unlock(&jm->lock);
    *ok = TRUE;
    if (wait && st == JOB_RUNNING) {
        double timeout = MIN(timeout_ms > 0 ? timeout_ms : 600000, 3600000) / 1000.0;
        if (p) {
            proc_wait(p, timeout, cancel);
            /* let the adopt thread record completion */
            for (int i = 0; i < 10 && !proc_running(p); i++) {
                g_mutex_lock(&jm->lock);
                gboolean fin = j->finished_at > 0;
                g_mutex_unlock(&jm->lock);
                if (fin) break;
                g_usleep(20000);
            }
        } else {
            gint64 deadline = g_get_monotonic_time() + (gint64)(timeout * G_USEC_PER_SEC);
            for (;;) {
                g_mutex_lock(&jm->lock);
                gboolean running = j->state == JOB_RUNNING && j->finished_at == 0;
                g_mutex_unlock(&jm->lock);
                if (!running || g_get_monotonic_time() >= deadline || (cancel && g_atomic_int_get(cancel))) break;
                g_usleep(200000);
            }
        }
    }
    g_mutex_lock(&jm->lock);
    j->collected = TRUE;
    st = j->state;
    char *result = g_strdup(j->result);
    gboolean finished = j->finished_at > 0;
    g_mutex_unlock(&jm->lock);
    JobState shown = st;
    if (st == JOB_RUNNING && finished) shown = JOB_COMPLETED;
    char *exit_note = g_strdup(result);
    if (shown == JOB_RUNNING && p && !proc_running(p)) {
        int code = proc_exit_code(p);
        shown = code == 0 ? JOB_COMPLETED : JOB_FAILED;
        g_free(exit_note);
        exit_note = g_strdup_printf("[exit code: %d]", code);
    }
    GString *s = g_string_new("");
    g_string_append_printf(s, "Job %s (%s): %s\n", id, kind_raw(j->kind), state_raw(shown));
    if (p) {
        char *nw = proc_read_new(p);
        if (!*nw) g_string_append(s, "(no new output)");
        else {
            char *t = utf8_suffix(nw, 30000);
            g_string_append(s, t);
            g_free(t);
        }
        g_free(nw);
        if (shown != JOB_RUNNING && exit_note) g_string_append_printf(s, "\n%s", exit_note);
        proc_unref(p);
    } else if (result) {
        g_string_append(s, result);
    } else {
        g_string_append(s, "(still running)");
    }
    g_free(result);
    g_free(exit_note);
    return g_string_free(s, FALSE);
}
