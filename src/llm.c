#include "llm.h"
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>

void llm_global_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

/* ---------------- draft ---------------- */

static DraftBlock *dblock_new(BlockType t) {
    DraftBlock *b = g_new0(DraftBlock, 1);
    b->type = t;
    b->text = g_string_new("");
    b->sig = g_string_new("");
    return b;
}

static void dblock_free(gpointer p) {
    DraftBlock *b = p;
    g_string_free(b->text, TRUE);
    g_string_free(b->sig, TRUE);
    g_free(b->id);
    g_free(b->name);
    g_free(b);
}

Draft *draft_new(void) {
    Draft *d = g_new0(Draft, 1);
    d->blocks = g_ptr_array_new_with_free_func(dblock_free);
    return d;
}

Draft *draft_copy(const Draft *s) {
    Draft *d = draft_new();
    for (guint i = 0; i < s->blocks->len; i++) {
        DraftBlock *a = g_ptr_array_index(s->blocks, i);
        DraftBlock *b = dblock_new(a->type);
        g_string_append_len(b->text, a->text->str, a->text->len);
        g_string_append_len(b->sig, a->sig->str, a->sig->len);
        b->id = g_strdup(a->id);
        b->name = g_strdup(a->name);
        g_ptr_array_add(d->blocks, b);
    }
    d->usage = s->usage;
    d->stop_reason = g_strdup(s->stop_reason);
    d->first_token_at = s->first_token_at;
    return d;
}

void draft_free(gpointer p) {
    Draft *d = p;
    if (!d) return;
    g_ptr_array_unref(d->blocks);
    g_free(d->stop_reason);
    g_free(d);
}

GPtrArray *draft_blocks(const Draft *d) {
    GPtrArray *out = g_ptr_array_new_with_free_func(block_free);
    for (guint i = 0; i < d->blocks->len; i++) {
        DraftBlock *b = g_ptr_array_index(d->blocks, i);
        switch (b->type) {
        case BLK_TEXT: g_ptr_array_add(out, block_text(b->text->str)); break;
        case BLK_THINKING: g_ptr_array_add(out, block_thinking(b->text->str, b->sig->len ? b->sig->str : NULL)); break;
        case BLK_TOOL_USE: g_ptr_array_add(out, block_tool_use(b->id, b->name, b->text->len ? b->text->str : "{}")); break;
        default: break;
        }
    }
    return out;
}

void llm_error_free(LLMError *e) {
    if (!e) return;
    g_free(e->message);
    g_free(e);
}

static LLMError *llm_error(const char *msg, int status, gboolean retryable) {
    LLMError *e = g_new0(LLMError, 1);
    e->message = g_strdup(msg);
    e->status = status;
    e->retryable = retryable;
    return e;
}

/* ---------------- request plumbing ---------------- */

typedef struct {
    GString *line;
    Draft *draft;
    GArray *index_map; /* wire index -> draft block index, -1 when ignored */
    long status;
    GString *body;     /* error body or full non-streaming body */
    gboolean streaming;
    gboolean stopped;
    LLMError *stream_err;
    DraftCallback cb;
    gpointer data;
    gint64 last_emit;
    volatile gint *cancel;
    double retry_after;
    CURL *curl;
    JsonParser *parser;
} Req;

static void emit(Req *r, gboolean force) {
    if (!r->cb) return;
    gint64 now = g_get_monotonic_time();
    if (force || now - r->last_emit > 45000) {
        r->last_emit = now;
        r->cb(r->draft, r->data);
    }
}

static void apply_usage(JsonObject *u, Usage *usage) {
    if (!u) return;
    if (jhas(u, "input_tokens")) usage->input = jint(u, "input_tokens", 0);
    if (jhas(u, "output_tokens")) usage->output = jint(u, "output_tokens", 0);
    if (jhas(u, "cache_read_input_tokens")) usage->cache_read = jint(u, "cache_read_input_tokens", 0);
    if (jhas(u, "cache_creation_input_tokens")) usage->cache_write = jint(u, "cache_creation_input_tokens", 0);
}

static void handle_event(Req *r, const char *payload) {
    if (!json_parser_load_from_data(r->parser, payload, -1, NULL)) return;
    JsonNode *root = json_parser_get_root(r->parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return;
    JsonObject *ev = json_node_get_object(root);
    const char *type = jstr(ev, "type");
    if (!type) return;
    Draft *d = r->draft;
    if (!strcmp(type, "message_start")) {
        apply_usage(jobj(jobj(ev, "message"), "usage"), &d->usage);
    } else if (!strcmp(type, "content_block_start")) {
        gint64 wi = jint(ev, "index", -1);
        JsonObject *cb = jobj(ev, "content_block");
        if (wi < 0 || !cb || wi > 10000) return;
        if (r->index_map->len <= (guint)wi) {
            guint old = r->index_map->len;
            g_array_set_size(r->index_map, wi + 1);
            for (guint i = old; i < r->index_map->len; i++) g_array_index(r->index_map, int, i) = -1;
        }
        const char *ct = jstr(cb, "type");
        DraftBlock *b = NULL;
        if (!g_strcmp0(ct, "thinking")) {
            b = dblock_new(BLK_THINKING);
            if (jstr(cb, "thinking")) g_string_append(b->text, jstr(cb, "thinking"));
            if (jstr(cb, "signature")) g_string_append(b->sig, jstr(cb, "signature"));
        } else if (!g_strcmp0(ct, "tool_use")) {
            b = dblock_new(BLK_TOOL_USE);
            b->id = jstr(cb, "id") ? g_strdup(jstr(cb, "id")) : uuid_new();
            b->name = g_strdup(jstr(cb, "name") ? jstr(cb, "name") : "");
        } else if (!g_strcmp0(ct, "text")) {
            b = dblock_new(BLK_TEXT);
            if (jstr(cb, "text")) g_string_append(b->text, jstr(cb, "text"));
        }
        if (b) {
            g_array_index(r->index_map, int, wi) = d->blocks->len;
            g_ptr_array_add(d->blocks, b);
        } else {
            g_array_index(r->index_map, int, wi) = -1;
        }
        emit(r, TRUE);
    } else if (!strcmp(type, "content_block_delta")) {
        gint64 wi = jint(ev, "index", -1);
        JsonObject *delta = jobj(ev, "delta");
        if (wi < 0 || (guint)wi >= r->index_map->len || !delta) return;
        int i = g_array_index(r->index_map, int, wi);
        if (i < 0) return;
        if (d->first_token_at == 0) d->first_token_at = now_ts();
        DraftBlock *b = g_ptr_array_index(d->blocks, i);
        const char *dt = jstr(delta, "type");
        if (!g_strcmp0(dt, "text_delta") && b->type == BLK_TEXT) {
            if (jstr(delta, "text")) g_string_append(b->text, jstr(delta, "text"));
        } else if (!g_strcmp0(dt, "thinking_delta") && b->type == BLK_THINKING) {
            if (jstr(delta, "thinking")) g_string_append(b->text, jstr(delta, "thinking"));
        } else if (!g_strcmp0(dt, "signature_delta") && b->type == BLK_THINKING) {
            if (jstr(delta, "signature")) g_string_append(b->sig, jstr(delta, "signature"));
        } else if (!g_strcmp0(dt, "input_json_delta") && b->type == BLK_TOOL_USE) {
            if (jstr(delta, "partial_json")) g_string_append(b->text, jstr(delta, "partial_json"));
        }
        emit(r, FALSE);
    } else if (!strcmp(type, "message_delta")) {
        JsonObject *delta = jobj(ev, "delta");
        if (jstr(delta, "stop_reason")) { g_free(d->stop_reason); d->stop_reason = g_strdup(jstr(delta, "stop_reason")); }
        apply_usage(jobj(ev, "usage"), &d->usage);
    } else if (!strcmp(type, "message_stop")) {
        r->stopped = TRUE;
    } else if (!strcmp(type, "error")) {
        JsonObject *e = jobj(ev, "error");
        const char *msg = jstr(e, "message") ? jstr(e, "message") : "Stream error";
        const char *et = jstr(e, "type") ? jstr(e, "type") : "";
        gboolean retry = !strcmp(et, "overloaded_error") || !strcmp(et, "api_error") || !strcmp(et, "rate_limit_error");
        if (!r->stream_err) r->stream_err = llm_error(msg, 0, retry);
    }
}

static void process_line(Req *r, char *line) {
    size_t n = strlen(line);
    if (n && line[n - 1] == '\r') line[--n] = 0;
    if (strncmp(line, "data:", 5) != 0) return;
    char *payload = line + 5;
    while (*payload == ' ' || *payload == '\t') payload++;
    if (!*payload || !strcmp(payload, "[DONE]")) return;
    handle_event(r, payload);
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    Req *r = ud;
    size_t len = size * nmemb;
    if (r->status == 0) curl_easy_getinfo(r->curl, CURLINFO_RESPONSE_CODE, &r->status);
    if (!r->streaming || r->status < 200 || r->status >= 300) {
        if (r->body->len < (r->streaming ? 64000 : 64 * 1024 * 1024)) g_string_append_len(r->body, ptr, len);
        return len;
    }
    g_string_append_len(r->line, ptr, len);
    char *start = r->line->str;
    char *nl;
    while ((nl = memchr(start, '\n', r->line->str + r->line->len - start))) {
        *nl = 0;
        process_line(r, start);
        start = nl + 1;
        if (r->stopped || r->stream_err) break;
    }
    g_string_erase(r->line, 0, start - r->line->str);
    if (r->stopped || r->stream_err) return 0; /* finish early */
    return len;
}

static size_t header_cb(char *buf, size_t size, size_t nitems, void *ud) {
    Req *r = ud;
    size_t len = size * nitems;
    if (len > 12 && g_ascii_strncasecmp(buf, "retry-after:", 12) == 0) {
        char *v = g_strndup(buf + 12, len - 12);
        r->retry_after = g_ascii_strtod(g_strstrip(v), NULL);
        g_free(v);
    }
    return len;
}

static int xfer_cb(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    Req *r = ud;
    return r->cancel && g_atomic_int_get(r->cancel) ? 1 : 0;
}

static LLMError *http_error(Req *r) {
    char *message = g_strdup_printf("DeepSeek request failed (HTTP %ld)", r->status);
    JsonNode *n = json_parse_str(r->body->str, r->body->len);
    const char *m = NULL;
    if (n && JSON_NODE_HOLDS_OBJECT(n)) m = jstr(jobj(json_node_get_object(n), "error"), "message");
    if (m) {
        g_free(message);
        message = g_strdup_printf("%s (HTTP %ld)", m, r->status);
    } else if (r->body->len) {
        char *snip = utf8_valid_dup(r->body->str, MIN(r->body->len, 400));
        char *t = g_strdup_printf("%s: %s", message, snip);
        g_free(message);
        g_free(snip);
        message = t;
    }
    if (n) json_node_unref(n);
    gboolean retry = r->status == 429 || r->status >= 500 || r->status == 408;
    LLMError *e = llm_error(message, r->status, retry);
    e->retry_after = r->retry_after;
    g_free(message);
    return e;
}

static CURL *make_curl(Req *r, const char *body, const char *session_id, gboolean stream, struct curl_slist **hdrs, char **key_out) {
    char *key = api_key_get();
    if (!key || !*key) {
        g_free(key);
        return NULL;
    }
    CURL *c = curl_easy_init();
    char *url = settings_messages_endpoint();
    curl_easy_setopt(c, CURLOPT_URL, url);
    g_free(url);
    struct curl_slist *h = NULL;
    char *kh = g_strconcat("x-api-key: ", key, NULL);
    h = curl_slist_append(h, kh);
    memset(kh, 0, strlen(kh));
    g_free(kh);
    h = curl_slist_append(h, "anthropic-version: 2023-06-01");
    h = curl_slist_append(h, "content-type: application/json");
    h = curl_slist_append(h, stream ? "accept: text/event-stream" : "accept: application/json");
    if (session_id) {
        char *sh = g_strconcat("x-deepseek-harness-session-id: ", session_id, NULL);
        h = curl_slist_append(h, sh);
        g_free(sh);
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "deepseek-harness-native-linux/1.0");
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(body));
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 300L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, r);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, r);
    *hdrs = h;
    memset(key, 0, strlen(key));
    g_free(key);
    (void)key_out;
    return c;
}

static void req_init(Req *r, gboolean streaming, volatile gint *cancel) {
    memset(r, 0, sizeof *r);
    r->line = g_string_new("");
    r->body = g_string_new("");
    r->index_map = g_array_new(FALSE, FALSE, sizeof(int));
    r->draft = draft_new();
    r->streaming = streaming;
    r->cancel = cancel;
    r->parser = json_parser_new();
}

static void req_clear(Req *r) {
    g_string_free(r->line, TRUE);
    g_string_free(r->body, TRUE);
    g_array_unref(r->index_map);
    if (r->draft) draft_free(r->draft);
    g_object_unref(r->parser);
    llm_error_free(r->stream_err);
}

static LLMError *no_key_error(void) {
    return llm_error("No DeepSeek API key configured. Open Settings → Models to add one, or set DEEPSEEK_API_KEY.", 0, FALSE);
}

Draft *llm_stream(const char *body, const char *session_id, volatile gint *cancel, DraftCallback on_event, gpointer data, LLMError **err) {
    *err = NULL;
    Req r;
    req_init(&r, TRUE, cancel);
    r.cb = on_event;
    r.data = data;
    struct curl_slist *h = NULL;
    CURL *c = make_curl(&r, body, session_id, TRUE, &h, NULL);
    if (!c) {
        req_clear(&r);
        *err = no_key_error();
        return NULL;
    }
    r.curl = c;
    CURLcode rc = curl_easy_perform(c);
    if (r.status == 0) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    Draft *result = NULL;
    if (cancel && g_atomic_int_get(cancel)) {
        LLMError *e = llm_error("Cancelled", 0, FALSE);
        e->cancelled = TRUE;
        *err = e;
    } else if (r.status && (r.status < 200 || r.status >= 300)) {
        *err = http_error(&r);
    } else if (r.stream_err) {
        *err = r.stream_err;
        r.stream_err = NULL;
    } else if (r.stopped) {
        result = r.draft;
        r.draft = NULL;
    } else if (rc != CURLE_OK) {
        char *m = g_strdup_printf("Stream interrupted: %s", curl_easy_strerror(rc));
        *err = llm_error(m, 0, TRUE);
        g_free(m);
    } else {
        *err = llm_error("Stream ended before message_stop", 0, TRUE);
    }
    /* Partial content stays available to the caller through the last callback. */
    curl_easy_cleanup(c);
    curl_slist_free_all(h);
    req_clear(&r);
    return result;
}

JsonNode *llm_complete(const char *body, const char *session_id, volatile gint *cancel, LLMError **err) {
    *err = NULL;
    Req r;
    req_init(&r, FALSE, cancel);
    struct curl_slist *h = NULL;
    CURL *c = make_curl(&r, body, session_id, FALSE, &h, NULL);
    if (!c) {
        req_clear(&r);
        *err = no_key_error();
        return NULL;
    }
    r.curl = c;
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    JsonNode *out = NULL;
    if (cancel && g_atomic_int_get(cancel)) {
        LLMError *e = llm_error("Cancelled", 0, FALSE);
        e->cancelled = TRUE;
        *err = e;
    } else if (rc != CURLE_OK) {
        *err = llm_error(curl_easy_strerror(rc), 0, TRUE);
    } else if (r.status < 200 || r.status >= 300) {
        *err = http_error(&r);
    } else {
        out = json_parse_str(r.body->str, r.body->len);
        if (!out || !JSON_NODE_HOLDS_OBJECT(out)) {
            if (out) json_node_unref(out);
            out = NULL;
            *err = llm_error("Invalid JSON response", r.status, FALSE);
        }
    }
    curl_easy_cleanup(c);
    curl_slist_free_all(h);
    req_clear(&r);
    return out;
}

char *llm_response_text(JsonNode *resp) {
    GString *s = g_string_new("");
    if (resp && JSON_NODE_HOLDS_OBJECT(resp)) {
        JsonArray *a = jarr(json_node_get_object(resp), "content");
        for (guint i = 0; a && i < json_array_get_length(a); i++) {
            JsonNode *n = json_array_get_element(a, i);
            if (!JSON_NODE_HOLDS_OBJECT(n)) continue;
            JsonObject *b = json_node_get_object(n);
            if (!g_strcmp0(jstr(b, "type"), "text") && jstr(b, "text")) g_string_append(s, jstr(b, "text"));
        }
    }
    return g_string_free(s, FALSE);
}

char *llm_title(const char *text, const char *model) {
    char *req = utf8_prefix(text, 2000);
    char *prompt = g_strconcat("Write a short title (3-6 words, no quotes, no trailing punctuation) that summarizes this request. "
                               "Reply with the title only, in the request's language.\n\nRequest:\n", req, NULL);
    g_free(req);
    JsonObject *o = jo_new();
    jo_str(o, "model", model);
    jo_int(o, "max_tokens", 60);
    jo_bool(o, "stream", FALSE);
    JsonObject *th = jo_new();
    jo_str(th, "type", "disabled");
    jo_obj(o, "thinking", th);
    JsonArray *msgs = json_array_new();
    JsonObject *m = jo_new();
    jo_str(m, "role", "user");
    JsonArray *content = json_array_new();
    JsonObject *t = jo_new();
    jo_str(t, "type", "text");
    jo_str(t, "text", prompt);
    json_array_add_object_element(content, t);
    jo_arr(m, "content", content);
    json_array_add_object_element(msgs, m);
    jo_arr(o, "messages", msgs);
    char *body = jobj_to_str(o);
    json_object_unref(o);
    g_free(prompt);
    LLMError *err = NULL;
    JsonNode *resp = llm_complete(body, NULL, NULL, &err);
    g_free(body);
    if (!resp) { llm_error_free(err); return NULL; }
    char *txt = llm_response_text(resp);
    json_node_unref(resp);
    char *trimmed = g_strstrip(txt);
    /* strip surrounding quote/punctuation characters */
    const char *strip = "\"'.#*";
    while (*trimmed && strchr(strip, *trimmed)) trimmed++;
    size_t n = strlen(trimmed);
    while (n > 0 && strchr(strip, trimmed[n - 1])) trimmed[--n] = 0;
    char *res = NULL;
    if (*trimmed) res = utf8_prefix(g_strstrip(trimmed), 80);
    g_free(txt);
    return res;
}
