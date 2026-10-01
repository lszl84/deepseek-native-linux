#pragma once
#include <glib.h>

/* Session files written by the Node DeepSeek Harness. */
GPtrArray *harness_candidates(void);
/* Imports every Harness session not imported yet; returns the number imported. Run off the main thread. */
int harness_import_all(void);
