#include "settings.h"
#include <libsecret/secret.h>
#include <string.h>

const ModelInfo MODELS[] = {
    { "deepseek-flash", "DeepSeek-V41-Flash", "Fast, capable default for everyday agentic work." },
    { "deepseek-v4-pro", "DeepSeek-V4-Pro", "Stronger agentic coding, knowledge, and difficult reasoning; suited to complex or quality-critical tasks at higher cost." },
};
const int N_MODELS = G_N_ELEMENTS(MODELS);

const char *model_name(const char *id) {
    for (int i = 0; i < N_MODELS; i++)
        if (!g_strcmp0(MODELS[i].id, id)) return MODELS[i].name;
    return id ? id : "";
}

const char *effort_raw(Effort e) {
    switch (e) {
    case EFFORT_OFF: return "off";
    case EFFORT_LOW: return "low";
    case EFFORT_MAX: return "max";
    default: return "high";
    }
}

const char *effort_label(Effort e) {
    switch (e) {
    case EFFORT_OFF: return "Off";
    case EFFORT_LOW: return "Low";
    case EFFORT_MAX: return "Max";
    default: return "High";
    }
}

Effort effort_parse(const char *s) {
    if (!g_strcmp0(s, "off")) return EFFORT_OFF;
    if (!g_strcmp0(s, "low")) return EFFORT_LOW;
    if (!g_strcmp0(s, "max")) return EFFORT_MAX;
    return EFFORT_HIGH;
}

const char *perm_raw(PermissionMode p) {
    switch (p) {
    case PERM_READ_ONLY: return "read-only";
    case PERM_FULL_ACCESS: return "danger-full-access";
    default: return "workspace-write";
    }
}

const char *perm_label(PermissionMode p) {
    switch (p) {
    case PERM_READ_ONLY: return "Read Only";
    case PERM_FULL_ACCESS: return "Full Access";
    default: return "Workspace Write";
    }
}

const char *perm_detail(PermissionMode p) {
    switch (p) {
    case PERM_READ_ONLY: return "Commands can read files; any write asks first.";
    case PERM_FULL_ACCESS: return "No sandbox and no approvals. Use with care.";
    default: return "Edits inside the workspace run freely; outside writes ask first.";
    }
}

const char *perm_icon(PermissionMode p) {
    switch (p) {
    case PERM_READ_ONLY: return "changes-prevent-symbolic";
    case PERM_FULL_ACCESS: return "dialog-warning-symbolic";
    default: return "security-high-symbolic";
    }
}

PermissionMode perm_parse(const char *s) {
    if (!g_strcmp0(s, "read-only")) return PERM_READ_ONLY;
    if (!g_strcmp0(s, "danger-full-access")) return PERM_FULL_ACCESS;
    return PERM_WORKSPACE_WRITE;
}

static Settings S;
static GMutex smutex;
static gboolean loaded;

static char *settings_path(void) { return g_build_filename(app_config_dir(), "settings.json", NULL); }

Settings *settings(void) {
    if (!loaded) settings_load();
    return &S;
}

static void set_defaults(void) {
    S.base_url = g_strdup("https://api.deepseek.com/anthropic");
    S.model = g_strdup("deepseek-flash");
    S.effort = EFFORT_HIGH;
    S.max_tokens = 256000;
    S.context_window = 1000000;
    S.appearance = APPEAR_SYSTEM;
    S.permission = PERM_WORKSPACE_WRITE;
    S.bash_timeout_ms = 120000;
    S.web_search = TRUE;
    S.subagents = TRUE;
    S.send_on_enter = TRUE;
    S.custom_instructions = g_strdup("");
    S.win_w = 1280;
    S.win_h = 820;
    S.sidebar_w = 270;
    S.sidebar_visible = TRUE;
}

void settings_load(void) {
    loaded = TRUE;
    set_defaults();
    char *path = settings_path();
    gsize len;
    char *data = read_file(path, &len);
    g_free(path);
    if (data) {
        JsonNode *n = json_parse_str(data, len);
        g_free(data);
        if (n && JSON_NODE_HOLDS_OBJECT(n)) {
            JsonObject *o = json_node_get_object(n);
            if (jstr(o, "baseURL")) { g_free(S.base_url); S.base_url = g_strdup(jstr(o, "baseURL")); }
            if (jstr(o, "model")) { g_free(S.model); S.model = g_strdup(jstr(o, "model")); }
            if (jstr(o, "effort")) S.effort = effort_parse(jstr(o, "effort"));
            S.max_tokens = jint(o, "maxTokens", S.max_tokens);
            S.context_window = jint(o, "contextWindow", S.context_window);
            if (jstr(o, "appearance")) {
                const char *a = jstr(o, "appearance");
                S.appearance = !strcmp(a, "light") ? APPEAR_LIGHT : !strcmp(a, "dark") ? APPEAR_DARK : APPEAR_SYSTEM;
            }
            if (jstr(o, "permissionMode")) S.permission = perm_parse(jstr(o, "permissionMode"));
            S.bash_timeout_ms = jint(o, "bashTimeoutMs", S.bash_timeout_ms);
            S.web_search = jbool(o, "webSearchEnabled", S.web_search);
            S.subagents = jbool(o, "subagentsEnabled", S.subagents);
            S.send_on_enter = jbool(o, "sendOnEnter", S.send_on_enter);
            if (jstr(o, "customInstructions")) { g_free(S.custom_instructions); S.custom_instructions = g_strdup(jstr(o, "customInstructions")); }
            S.win_w = jint(o, "windowWidth", S.win_w);
            S.win_h = jint(o, "windowHeight", S.win_h);
            S.sidebar_w = jint(o, "sidebarWidth", S.sidebar_w);
            S.sidebar_visible = jbool(o, "sidebarVisible", S.sidebar_visible);
        }
        if (n) json_node_unref(n);
    }
    const char *base = g_getenv("DEEPSEEK_BASE_URL");
    if (base && *base) { g_free(S.base_url); S.base_url = g_strdup(base); }
    const char *cw = g_getenv("DSN_CONTEXT_WINDOW");
    if (cw && *cw) S.context_window = g_ascii_strtoll(cw, NULL, 10);
    const char *m = g_getenv("DSN_MODEL");
    if (m && *m) { g_free(S.model); S.model = g_strdup(m); }
}

void settings_save(void) {
    JsonObject *o = jo_new();
    g_mutex_lock(&smutex);
    jo_str(o, "baseURL", S.base_url);
    jo_str(o, "model", S.model);
    jo_str(o, "effort", effort_raw(S.effort));
    jo_int(o, "maxTokens", S.max_tokens);
    jo_int(o, "contextWindow", S.context_window);
    jo_str(o, "appearance", S.appearance == APPEAR_LIGHT ? "light" : S.appearance == APPEAR_DARK ? "dark" : "system");
    jo_str(o, "permissionMode", perm_raw(S.permission));
    jo_int(o, "bashTimeoutMs", S.bash_timeout_ms);
    jo_bool(o, "webSearchEnabled", S.web_search);
    jo_bool(o, "subagentsEnabled", S.subagents);
    jo_bool(o, "sendOnEnter", S.send_on_enter);
    jo_str(o, "customInstructions", S.custom_instructions);
    jo_int(o, "windowWidth", S.win_w);
    jo_int(o, "windowHeight", S.win_h);
    jo_int(o, "sidebarWidth", S.sidebar_w);
    jo_bool(o, "sidebarVisible", S.sidebar_visible);
    g_mutex_unlock(&smutex);
    JsonNode *n = jnode_obj(o);
    json_object_unref(o);
    char *s = json_to_str(n, TRUE);
    json_node_unref(n);
    char *path = settings_path();
    write_file_atomic(path, s, -1, 0600);
    g_free(path);
    g_free(s);
}

static void set_str(char **field, const char *v) {
    char *n = g_strdup(v ? v : "");
    g_mutex_lock(&smutex);
    char *old = *field;
    *field = n;
    g_mutex_unlock(&smutex);
    g_free(old);
}

void settings_set_model(const char *v) { set_str(&settings()->model, v); }
void settings_set_base_url(const char *v) { set_str(&settings()->base_url, v); }
void settings_set_custom(const char *v) { set_str(&settings()->custom_instructions, v); }

char *settings_dup_model(void) {
    g_mutex_lock(&smutex);
    char *m = g_strdup(settings()->model);
    g_mutex_unlock(&smutex);
    return m;
}

char *settings_dup_custom(void) {
    g_mutex_lock(&smutex);
    char *m = g_strdup(settings()->custom_instructions);
    g_mutex_unlock(&smutex);
    return m;
}

char *settings_messages_endpoint(void) {
    g_mutex_lock(&smutex);
    char *base = str_trim(settings()->base_url);
    g_mutex_unlock(&smutex);
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') base[--n] = 0;
    char *r = str_has_suffix(base, "/v1") ? g_strconcat(base, "/messages", NULL) : g_strconcat(base, "/v1/messages", NULL);
    g_free(base);
    return r;
}

/* ---------------- API key ---------------- */

static const SecretSchema generic_schema = {
    "org.freedesktop.Secret.Generic", SECRET_SCHEMA_DONT_MATCH_NAME,
    {
        { "service", SECRET_SCHEMA_ATTRIBUTE_STRING },
        { "user", SECRET_SCHEMA_ATTRIBUTE_STRING },
        { NULL, 0 },
    }
};

#define APP_SERVICE "deepseek-native/deepseek"

static GMutex kmutex;
static char *cached_key;
static gboolean key_resolved;
static const char *key_source = "none";

static char *keyring_lookup(const char *service) {
    GError *err = NULL;
    char *s = secret_password_lookup_sync(&generic_schema, NULL, &err, "service", service, "user", "api_key", NULL);
    if (err) { dlog("keyring lookup %s failed: %s", service, err->message); g_error_free(err); return NULL; }
    if (!s) return NULL;
    char *r = str_trim(s);
    secret_password_free(s);
    if (!*r) { g_free(r); return NULL; }
    return r;
}

static char *dsh_credentials_key(void) {
    char *path = g_build_filename(g_get_home_dir(), ".dsh", ".credentials.yaml", NULL);
    char *text = read_file(path, NULL);
    g_free(path);
    if (!text) return NULL;
    char *result = NULL;
    char **lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *t = g_strstrip(lines[i]);
        if (g_str_has_prefix(t, "DEEPSEEK_API_KEY:")) {
            char *v = g_strstrip(t + strlen("DEEPSEEK_API_KEY:"));
            size_t n = strlen(v);
            if (n >= 2 && (v[0] == '"' || v[0] == '\'')) { v[n - 1] = 0; v++; }
            if (*v) result = g_strdup(v);
            break;
        }
    }
    g_strfreev(lines);
    g_free(text);
    return result;
}

static void resolve_locked(void) {
    if (key_resolved) return;
    key_resolved = TRUE;
    char *k = NULL;
    if (!g_getenv("DSN_NO_KEYRING")) k = keyring_lookup(APP_SERVICE);
    if (k) { key_source = "keyring (DeepSeek Native)"; cached_key = k; return; }
    const char *env = g_getenv("DEEPSEEK_API_KEY");
    if (env && *env) { key_source = "DEEPSEEK_API_KEY environment variable"; cached_key = g_strdup(env); return; }
    k = dsh_credentials_key();
    if (k) { key_source = "~/.dsh/.credentials.yaml"; cached_key = k; return; }
    key_source = "none";
}

char *api_key_get(void) {
    g_mutex_lock(&kmutex);
    resolve_locked();
    char *k = g_strdup(cached_key);
    g_mutex_unlock(&kmutex);
    return k;
}

gboolean api_key_present(void) {
    g_mutex_lock(&kmutex);
    resolve_locked();
    gboolean r = cached_key && *cached_key;
    g_mutex_unlock(&kmutex);
    return r;
}

const char *api_key_source(void) {
    g_mutex_lock(&kmutex);
    resolve_locked();
    const char *s = key_source;
    g_mutex_unlock(&kmutex);
    return s;
}

gboolean api_key_store(const char *key) {
    char *k = str_trim(key);
    GError *err = NULL;
    gboolean ok;
    if (!*k) {
        ok = secret_password_clear_sync(&generic_schema, NULL, &err, "service", APP_SERVICE, "user", "api_key", NULL);
    } else {
        ok = secret_password_store_sync(&generic_schema, SECRET_COLLECTION_DEFAULT, "DeepSeek Native API key", k, NULL, &err,
                                        "service", APP_SERVICE, "user", "api_key", NULL);
    }
    if (err) { g_error_free(err); ok = FALSE; }
    g_mutex_lock(&kmutex);
    g_free(cached_key);
    cached_key = NULL;
    key_resolved = FALSE;
    if (*k && !ok) {
        /* Keyring unavailable: keep it for this run only. */
        cached_key = g_strdup(k);
        key_resolved = TRUE;
        key_source = "this session only (keyring unavailable)";
    }
    g_mutex_unlock(&kmutex);
    g_free(k);
    return ok;
}

typedef struct { void (*done)(gpointer); gpointer data; } ResolveCtx;

static gpointer resolve_thread(gpointer p) {
    ResolveCtx *c = p;
    api_key_present();
    if (c->done) main_async(c->done, c->data);
    g_free(c);
    return NULL;
}

void api_key_resolve_async(void (*done)(gpointer), gpointer data) {
    ResolveCtx *c = g_new(ResolveCtx, 1);
    c->done = done;
    c->data = data;
    g_thread_unref(g_thread_new("key", resolve_thread, c));
}
