#pragma once
#include "process.h"

typedef enum { JOB_BASH, JOB_SUBAGENT } JobKind;
typedef enum { JOB_RUNNING, JOB_COMPLETED, JOB_FAILED, JOB_KILLED } JobState;

typedef struct Job Job;
typedef struct JobManager JobManager;

/* Body of an asynchronous job; returns the result text and sets *ok. Runs on its own thread. */
typedef char *(*JobBody)(gpointer data, volatile gint *cancel, gboolean *ok);

JobManager *jobs_new(void);
void jobs_free(JobManager *jm);
void jobs_set_on_finish(JobManager *jm, void (*fn)(const char *id, const char *kind, const char *state, const char *label, gpointer data), gpointer data);
/* Adopts a running process as a background job. Returns the job id (owned by the manager). */
const char *jobs_adopt(JobManager *jm, ChildProcess *p, const char *label);
const char *jobs_start(JobManager *jm, JobKind kind, const char *label, JobBody body, gpointer data, GDestroyNotify free_data);
char *jobs_kill(JobManager *jm, const char *id);
void jobs_kill_all(JobManager *jm);
char *jobs_list(JobManager *jm);
/* Returns text; *ok false when the id is unknown. */
char *jobs_output(JobManager *jm, const char *id, gboolean wait, gint64 timeout_ms, volatile gint *cancel, gboolean *ok);
int jobs_running_count(JobManager *jm);
/* Job threads still alive (including killed jobs that have not exited yet). */
int jobs_thread_count(JobManager *jm);
