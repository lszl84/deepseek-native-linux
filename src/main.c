#include "util.h"
#include "settings.h"
#include "llm.h"
#include "headless.h"
#include "app.h"
#include <string.h>
#include <locale.h>
#include <stdio.h>
#include <gtk/gtk.h>

#ifndef DSN_VERSION
#define DSN_VERSION "dev"
#endif

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    debug_logging = g_getenv("DSN_DEBUG") != NULL;
    main_init();
    llm_global_init();
    settings_load();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless") && i + 1 < argc) {
            const char *cwd = NULL, *perm = NULL;
            for (int j = 1; j < argc; j++) {
                if (!strcmp(argv[j], "--cwd") && j + 1 < argc) cwd = argv[j + 1];
                if (!strcmp(argv[j], "--permission") && j + 1 < argc) perm = argv[j + 1];
            }
            char *here = g_get_current_dir();
            int r = headless_run(argv[i + 1], cwd ? cwd : here, perm);
            g_free(here);
            return r;
        }
        if (!strcmp(argv[i], "--version")) {
            printf("deepseek-native %s (GTK %u.%u.%u)\n", DSN_VERSION, gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version());
            return 0;
        }
    }
    return app_run(argc, argv);
}
