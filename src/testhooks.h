#pragma once
#include <gtk/gtk.h>
#include "chat.h"
#include "agent.h"

/* What the test hooks need from the app; see testhooks.c. */
typedef struct {
    GtkWidget *(*window)(void);
    Chat *chat;
    void (*open_session)(const char *id);
    void (*new_session)(const char *workspace);
    void (*open_settings)(void);
    void (*quit)(void);
    AgentEventFn agent_event;
} TestHooksCtx;

#ifdef DSN_TEST_HOOKS
void testhooks_run(const TestHooksCtx *ctx);
void testhooks_window_size(int *w, int *h);
gboolean testhooks_suppress_key_prompt(void);
#else
static inline gboolean testhooks_suppress_key_prompt(void) { return FALSE; }
#endif
