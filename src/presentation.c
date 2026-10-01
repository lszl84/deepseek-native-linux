#include "presentation.h"
#include <string.h>

ToolCategory tool_category(const char *n) {
    if (!n) return CAT_OTHER;
    if (!strcmp(n, "bash") || !strcmp(n, "job_output") || !strcmp(n, "job_kill") || !strcmp(n, "job_list")) return CAT_COMMAND;
    if (!strcmp(n, "read") || !strcmp(n, "read_image")) return CAT_READ;
    if (!strcmp(n, "edit")) return CAT_EDIT;
    if (!strcmp(n, "write")) return CAT_WRITE;
    if (!strcmp(n, "grep") || !strcmp(n, "glob")) return CAT_SEARCH;
    if (!strcmp(n, "todo_write")) return CAT_PLAN;
    if (!strcmp(n, "web_fetch") || !strcmp(n, "web_search")) return CAT_WEB;
    if (!strcmp(n, "subagent")) return CAT_DELEGATE;
    if (!strcmp(n, "ask_user_question")) return CAT_ASK;
    return CAT_OTHER;
}

const char *tool_display_name(const char *n) {
    static const char *map[][2] = {
        { "bash", "Bash" }, { "read", "Read" }, { "read_image", "View image" }, { "edit", "Edit" }, { "write", "Write" },
        { "grep", "Grep" }, { "glob", "Glob" }, { "todo_write", "Update to-do list" }, { "web_fetch", "Fetch" },
        { "web_search", "Web search" }, { "subagent", "Subagent" }, { "ask_user_question", "Ask user" },
        { "present", "Present files" }, { "job_output", "Job output" }, { "job_kill", "Stop job" }, { "job_list", "List jobs" },
    };
    for (size_t i = 0; n && i < G_N_ELEMENTS(map); i++)
        if (!strcmp(map[i][0], n)) return map[i][1];
    return n ? n : "";
}

const char *tool_icon(const char *n) {
    static const char *map[][2] = {
        { "bash", "utilities-terminal-symbolic" }, { "read", "text-x-generic-symbolic" }, { "read_image", "image-x-generic-symbolic" },
        { "edit", "document-edit-symbolic" }, { "write", "document-new-symbolic" }, { "grep", "edit-find-symbolic" },
        { "glob", "folder-saved-search-symbolic" }, { "todo_write", "view-list-symbolic" }, { "web_fetch", "web-browser-symbolic" },
        { "web_search", "system-search-symbolic" }, { "subagent", "system-users-symbolic" }, { "ask_user_question", "help-faq-symbolic" },
        { "present", "x-office-document-symbolic" }, { "job_output", "view-paged-symbolic" }, { "job_list", "view-paged-symbolic" },
        { "job_kill", "process-stop-symbolic" },
    };
    for (size_t i = 0; n && i < G_N_ELEMENTS(map); i++)
        if (!strcmp(map[i][0], n)) return map[i][1];
    return "applications-system-symbolic";
}

char *tool_summary(const char *n, JsonObject *in, const char *cwd) {
    if (!n || !in) return g_strdup("");
    if (!strcmp(n, "bash")) {
        const char *d = jstr(in, "description");
        if (d && *d) return g_strdup(d);
        return first_line(jstr(in, "command"), 200);
    }
    if (!strcmp(n, "read") || !strcmp(n, "write") || !strcmp(n, "edit") || !strcmp(n, "read_image")) return short_path(jstr(in, "file_path"), cwd);
    if (!strcmp(n, "grep")) {
        GString *s = g_string_new(jstr(in, "pattern") ? jstr(in, "pattern") : "");
        if (jstr(in, "path")) {
            char *p = short_path(jstr(in, "path"), cwd);
            g_string_append_printf(s, " in %s", p);
            g_free(p);
        }
        if (jstr(in, "include")) g_string_append_printf(s, " (%s)", jstr(in, "include"));
        return g_string_free(s, FALSE);
    }
    if (!strcmp(n, "glob")) return g_strdup(jstr(in, "pattern") ? jstr(in, "pattern") : "");
    if (!strcmp(n, "todo_write")) {
        JsonArray *a = jarr(in, "todos");
        if (!a || !json_array_get_length(a)) return g_strdup("");
        int done = 0;
        const char *cur = NULL;
        for (guint i = 0; i < json_array_get_length(a); i++) {
            JsonObject *t = json_array_get_object_element(a, i);
            if (!t) continue;
            if (!g_strcmp0(jstr(t, "status"), "completed")) done++;
            if (!cur && !g_strcmp0(jstr(t, "status"), "in_progress")) cur = jstr(t, "content");
        }
        return cur ? g_strdup_printf("%d/%u completed · %s", done, json_array_get_length(a), cur)
                   : g_strdup_printf("%d/%u completed", done, json_array_get_length(a));
    }
    if (!strcmp(n, "web_fetch")) return g_strdup(jstr(in, "url") ? jstr(in, "url") : "");
    if (!strcmp(n, "web_search")) {
        JsonArray *a = jarr(in, "queries");
        GString *s = g_string_new("");
        for (guint i = 0; a && i < json_array_get_length(a); i++) {
            const char *q = json_node_get_string(json_array_get_element(a, i));
            if (!q) continue;
            if (s->len) g_string_append(s, " · ");
            g_string_append(s, q);
        }
        if (!s->len && jstr(in, "query")) g_string_append(s, jstr(in, "query"));
        return g_string_free(s, FALSE);
    }
    if (!strcmp(n, "subagent")) return g_strdup(jstr(in, "description") ? jstr(in, "description") : "");
    if (!strcmp(n, "ask_user_question")) {
        JsonArray *a = jarr(in, "questions");
        JsonObject *q = a && json_array_get_length(a) ? json_array_get_object_element(a, 0) : NULL;
        return g_strdup(q && jstr(q, "question") ? jstr(q, "question") : "");
    }
    if (!strcmp(n, "present")) {
        JsonArray *a = jarr(in, "files");
        GString *s = g_string_new("");
        for (guint i = 0; a && i < json_array_get_length(a); i++) {
            JsonObject *f = json_array_get_object_element(a, i);
            if (!f || !jstr(f, "path")) continue;
            if (s->len) g_string_append(s, ", ");
            g_string_append(s, path_base(jstr(f, "path")));
        }
        return g_string_free(s, FALSE);
    }
    if (!strcmp(n, "job_output") || !strcmp(n, "job_kill")) return g_strdup(jstr(in, "job_id") ? jstr(in, "job_id") : "");
    return g_strdup("");
}

char *tool_group_title(const ToolCategory *cats, int n, gboolean running) {
    ToolCategory order[16];
    int no = 0;
    for (int i = 0; i < n; i++) {
        gboolean seen = FALSE;
        for (int j = 0; j < no; j++)
            if (order[j] == cats[i]) seen = TRUE;
        if (!seen && no < 16) order[no++] = cats[i];
    }
    GString *s = g_string_new("");
    for (int i = 0; i < no && i < 2; i++) {
        const char *ph;
        switch (order[i]) {
        case CAT_COMMAND: ph = running ? "running commands" : "ran commands"; break;
        case CAT_READ: ph = running ? "reading files" : "read files"; break;
        case CAT_EDIT: ph = running ? "editing files" : "edited files"; break;
        case CAT_WRITE: ph = running ? "writing files" : "wrote files"; break;
        case CAT_SEARCH: ph = running ? "searching" : "searched the workspace"; break;
        case CAT_PLAN: ph = running ? "updating the plan" : "updated the plan"; break;
        case CAT_WEB: ph = running ? "browsing the web" : "browsed the web"; break;
        case CAT_DELEGATE: ph = running ? "delegating work" : "delegated work"; break;
        case CAT_ASK: ph = running ? "asking a question" : "asked a question"; break;
        default: ph = running ? "using tools" : "used tools"; break;
        }
        if (i) g_string_append(s, " and ");
        g_string_append(s, ph);
    }
    if (no > 2) g_string_append(s, running ? ", and more" : " and more");
    if (s->len) s->str[0] = g_ascii_toupper(s->str[0]);
    return g_string_free(s, FALSE);
}

const char *tool_group_icon(const ToolCategory *cats, int n) {
    if (n <= 0) return "starred-symbolic";
    switch (cats[0]) {
    case CAT_COMMAND: return "utilities-terminal-symbolic";
    case CAT_READ: return "text-x-generic-symbolic";
    case CAT_EDIT:
    case CAT_WRITE: return "document-edit-symbolic";
    case CAT_SEARCH: return "edit-find-symbolic";
    case CAT_PLAN: return "view-list-symbolic";
    case CAT_WEB: return "web-browser-symbolic";
    case CAT_DELEGATE: return "system-users-symbolic";
    case CAT_ASK: return "help-faq-symbolic";
    default: return "applications-system-symbolic";
    }
}
